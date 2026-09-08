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
static uint16_t inode_links = 3U;
static unsigned renames;
static uint64_t pending_size;
static unsigned sync_refusals;
static unsigned file_sync_calls;
static unsigned publication_calls;
static unsigned held_unlink_calls;
static unsigned stat_refusals;
static unsigned appends;
static uint16_t changed_mode;
static uint16_t directory_mode;
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
static unsigned capacity_queries;
static bool lstat_symbolic;
static uint8_t prepared_flags;
static uint16_t prepared_mode;
static unsigned prepared_opens;
static unsigned unmount_refusals;
static unsigned live_mounts = 1U;
static uint32_t logical_block_bytes = 4096U;
static phipfs_handle callback_handle;
static unsigned close_callback_kind;
static phipfs_handle moved_cursor_handle;

static void close_from_callback(unsigned kind)
{
    if (close_callback_kind != kind) return;
    close_callback_kind = 0U;
    uint64_t position = 99U;
    struct ext4_handle_state *held;
    const size_t index = (size_t)((callback_handle & 0xffU) - 1U);

    assert(ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    assert(ext4_backend_seek(callback_handle, 0, PHIPFS_SEEK_START, &position) == PHIPFS_STATUS_BUSY);
    assert(ext4_backend_close(callback_handle) == PHIPFS_STATUS_OK);
    assert(handle_state(callback_handle, &held) == PHIPFS_STATUS_STALE_HANDLE);
    assert(ext4_backend_close(callback_handle) == PHIPFS_STATUS_STALE_HANDLE);
    assert(ext4_handles[index].active && ext4_handles[index].closing);
    assert(ext4_handles[index].inode == 42U);
}

int32_t phipia_ext4_free_bytes(uintptr_t mounted, uint64_t *bytes)
{
    assert(mounted == 1U && bytes != NULL);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    const unsigned before = ++capacity_queries;
    const struct phipfs_drive_info drive = ext4_backend_drive(PHIPFS_VOLUME_DATA);
    assert(drive.mounted == ext4_mounts[PHIPFS_VOLUME_DATA].active);
    assert(capacity_queries == before);
    *bytes = 123U * 4096U;
    return PHIPIA_EXT4_STATUS_OK;
}

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
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].detaching);
    phipfs_handle attempted = 0U;
    const unsigned before = opens;
    assert(ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ, &attempted) == PHIPFS_STATUS_BUSY);
    assert(attempted == 0U && opens == before);
    if (unmount_refusals != 0U) { --unmount_refusals; return PHIPIA_EXT4_STATUS_IO; }
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
    close_from_callback(5U);
    assert(live_snapshots == 1U);
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
    assert(maximum_size == PHIPIA_EXT4_MAX_MUTABLE_FILE_BYTES);
    ++appends;
    if (permanent_status != PHIPIA_EXT4_STATUS_OK) return permanent_status;
    if (disk_size > maximum_size || source_bytes > maximum_size - disk_size) {
        return PHIPIA_EXT4_STATUS_RANGE;
    }
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
    close_from_callback(3U);
    close_from_callback(6U);
    if (moved_cursor_handle != 0U) {
        struct ext4_handle_state *cursor;
        assert(handle_state(moved_cursor_handle, &cursor) == PHIPFS_STATUS_OK);
        /* Model a cursor publication between the initial range check and the
         * storage call. The owned check must use this current cursor. */
        cursor->offset = PHIPIA_EXT4_MAX_MUTABLE_FILE_BYTES;
        moved_cursor_handle = 0U;
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
    close_from_callback(4U);
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
    metadata->mode = 0100640U;
    metadata->uid = 70000U;
    metadata->gid = 90000U;
    metadata->links = inode_links;
    metadata->atime_seconds = -1;
    metadata->atime_nanos = 123U;
    metadata->mtime_seconds = INT64_C(2147483648);
    metadata->mtime_nanos = 999999999U;
    metadata->ctime_seconds = INT32_MIN;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_prepare_open(uintptr_t mounted, const uint8_t *path, size_t length,
    uint8_t access, uint8_t flags, uint16_t mode, struct phipia_ext4_metadata *metadata)
{
    assert(access >= PHIPFS_ACCESS_READ && access <= PHIPFS_ACCESS_READ_WRITE);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable == (flags != 0U));
    prepared_flags = flags;
    prepared_mode = mode;
    ++prepared_opens;
    if (flags != 0U && permanent_status != PHIPIA_EXT4_STATUS_OK) return permanent_status;
    if ((flags & PHIPFS_OPEN_TRUNCATE) != 0U) disk_size = 0U;
    return phipia_ext4_stat(mounted, path, length, metadata);
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
    close_from_callback(1U);
    *count = 0U;
    if (pending) return PHIPIA_EXT4_STATUS_IO;
    *count = offset >= disk_size ? 0U : (size_t)(disk_size - offset);
    if (*count > capacity) *count = capacity;
    memset(output, 0x55, *count);
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_write_inode(uintptr_t mounted, uint64_t inode, uint64_t offset,
    const uint8_t *source, size_t length, size_t *count)
{
    assert(mounted == 1U && inode == 42U && source != NULL && ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    close_from_callback(2U);
    *count = length;
    if (offset + length > disk_size) disk_size = offset + length;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_publish_file(uintptr_t mounted, const uint8_t *source, size_t source_length,
    const uint8_t *destination, size_t destination_length, uint64_t inode,
    const uint64_t *open_inodes, size_t open_count)
{
    assert(mounted == 1U && inode == 42U && ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    assert(source_length == 4U && memcmp(source, "file", 4U) == 0);
    assert(destination_length == 5U && memcmp(destination, "moved", 5U) == 0);
    assert(open_count == 2U && open_inodes[0] == 42U && open_inodes[1] == 42U);
    ++publication_calls;
    return permanent_status;
}

int32_t phipia_ext4_unlink_held_file(uintptr_t mounted, const uint8_t *path,
    size_t length, uint64_t inode, const uint64_t *open_inodes, size_t open_count)
{
    assert(mounted == 1U && inode == 42U && ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    assert(length == 4U && memcmp(path, "file", 4U) == 0);
    assert(open_count == 2U && open_inodes[0] == 42U && open_inodes[1] == 42U);
    ++held_unlink_calls;
    return permanent_status;
}

int32_t phipia_ext4_sync(uintptr_t mounted, const uint64_t *open_inodes, size_t open_count)
{
    ++file_sync_calls;
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
    const int32_t status = phipia_ext4_stat(mounted, path, path_bytes, metadata);
    if (status == PHIPIA_EXT4_STATUS_OK && lstat_symbolic) {
        metadata->file_type = PHIPIA_EXT4_FILE_SYMLINK;
        metadata->mode = 0120777U;
        metadata->inode = 84U;
        metadata->size = 10U;
    }
    return status;
}

int32_t phipia_ext4_create_directory_mode(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, uint16_t mode)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    directory_mode = mode;
    return permanent_status;
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

int32_t phipia_ext4_remove_directory_probe(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint64_t *open_inodes, size_t open_count)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    assert(open_count == 1U && open_inodes[0] == 42U);
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
    assert(map_status(PHIPIA_EXT4_STATUS_SYMLINK_LOOP) == PHIPFS_STATUS_SYMLINK_LOOP);
    assert(map_status(PHIPIA_EXT4_STATUS_NAME_TOO_LONG) == PHIPFS_STATUS_NAME_TOO_LONG);
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
        uint8_t output = 0xa5U;
        size_t read = 99U;
        uint64_t end_position = 99U;
        assert(ext4_backend_pread(first, &output, 1U, 8192U, &read) == PHIPFS_STATUS_IO);
        assert(read == 0U && output == 0xa5U);
        assert(ext4_backend_seek(first, 0, PHIPFS_SEEK_END, &end_position) == PHIPFS_STATUS_IO);
        assert(end_position == 0U && state->offset == 0U);
        assert(opens == closes && !ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    }
    expect_published_size_before_close = true;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 101U) == PHIPFS_STATUS_OK);
    assert(!expect_published_size_before_close);
    assert(truncates == 3U && stats == 3U);
    assert(!reenter_on_open);
    const unsigned counted = capacity_queries;
    const unsigned before_drive = opens;
    assert(ext4_backend_drive(PHIPFS_VOLUME_DATA).free_bytes == 123U * 4096U);
    assert(capacity_queries == counted && opens == before_drive);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    assert(handle_state(second, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    permanent_status = PHIPIA_EXT4_STATUS_FULL;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 100U) == PHIPFS_STATUS_FULL);
    permanent_status = PHIPIA_EXT4_STATUS_READ_ONLY;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 100U) == PHIPFS_STATUS_READ_ONLY);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    stat_refusals = 1U;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 103U) == PHIPFS_STATUS_IO);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    uint8_t grown_byte = 0U;
    size_t grown_count = 0U;
    uint64_t grown_end = 0U;
    assert(ext4_backend_pread(first, &grown_byte, 1U, 102U, &grown_count) == PHIPFS_STATUS_OK);
    assert(grown_count == 1U && grown_byte == 0x55U);
    assert(ext4_backend_seek(first, 0, PHIPFS_SEEK_END, &grown_end) == PHIPFS_STATUS_OK);
    assert(grown_end == 103U && state->size == 103U);
    assert(ext4_backend_seek(first, 0, PHIPFS_SEEK_START, &grown_end) == PHIPFS_STATUS_OK);
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 101U) == PHIPFS_STATUS_OK);
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
    const uint64_t maximum = PHIPIA_EXT4_MAX_MUTABLE_FILE_BYTES;
    assert(maximum == UINT64_C(64) * 1024U * 1024U);
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", maximum - 2U) == PHIPFS_STATUS_OK);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == maximum - 2U);
    assert(ext4_backend_seek(second, (int64_t)(maximum - 2U), PHIPFS_SEEK_START, &position) == PHIPFS_STATUS_OK);
    assert(ext4_backend_write(second, (const uint8_t *)"x", 1U, &written) == PHIPFS_STATUS_OK && written == 1U);
    assert(ext4_backend_append(second, (const uint8_t *)"y", 1U, &written) == PHIPFS_STATUS_OK && written == 1U);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == maximum && state->offset == 1U);
    const unsigned before_range = opens;
    assert(ext4_backend_write(second, (const uint8_t *)"z", 1U, &written) == PHIPFS_STATUS_RANGE && written == 0U);
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", maximum + 1U) == PHIPFS_STATUS_RANGE);
    assert(ext4_backend_ftruncate(second, UINT64_MAX) == PHIPFS_STATUS_RANGE);
    assert(opens == before_range);
    assert(ext4_backend_append(second, (const uint8_t *)"z", 1U, &written) == PHIPFS_STATUS_RANGE && written == 0U);
    assert(disk_size == maximum && opens == closes);
    assert(ext4_backend_ftruncate(second, maximum) == PHIPFS_STATUS_OK);
    assert(ext4_backend_seek(second, 0, PHIPFS_SEEK_START, &position) == PHIPFS_STATUS_OK);
    moved_cursor_handle = second;
    assert(ext4_backend_write(second, (const uint8_t *)"z", 1U, &written) == PHIPFS_STATUS_RANGE && written == 0U);
    assert(moved_cursor_handle == 0U && disk_size == maximum && opens == closes);
    assert(ext4_backend_ftruncate(second, 5U) == PHIPFS_STATUS_OK);
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
    assert(ext4_backend_rmdir(PHIPFS_VOLUME_DATA, "file") == PHIPFS_STATUS_OK);
    assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    assert(last_sync_open_count == 1U);
    assert(ext4_backend_directory_close(first) == PHIPFS_STATUS_OK);
    assert(last_sync_open_count == 0U && !ext4_mounts[PHIPFS_VOLUME_DATA].orphan_cleanup_pending);
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
    for (unsigned kind = 1U; kind <= 4U; ++kind) {
        assert(allocate_handle(PHIPFS_VOLUME_DATA, "file", 42U, disk_size,
            kind == 2U ? PHIPFS_ACCESS_WRITE : PHIPFS_ACCESS_READ,
            false, 0U, &callback_handle) == PHIPFS_STATUS_OK);
        const size_t index = (size_t)((callback_handle & 0xffU) - 1U);
        close_callback_kind = kind;
        open_reports_failure = kind == 3U;
        close_reports_failure = kind == 4U;
        uint8_t value = 0U;
        size_t count = 0U;
        const enum phipfs_status result = kind == 2U ?
            ext4_backend_write(callback_handle, (const uint8_t *)"race", 4U, &count) :
            ext4_backend_read(callback_handle, &value, 1U, &count);
        assert(result == (kind >= 3U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK));
        assert(close_callback_kind == 0U);
        assert(handle_state(callback_handle, &state) == PHIPFS_STATUS_STALE_HANDLE);
        assert(!ext4_handles[index].active && !ext4_handles[index].closing);
        assert(ext4_handles[index].offset == 0U && ext4_handles[index].inode == 0U);
        assert(!volume_has_open_handles(PHIPFS_VOLUME_DATA));
        open_reports_failure = false;
        close_reports_failure = false;
        assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    }
    for (unsigned operation = 0U; operation < 5U; ++operation) {
        assert(allocate_handle(PHIPFS_VOLUME_DATA, "file", 42U, disk_size,
            PHIPFS_ACCESS_READ_WRITE, false, 0U, &callback_handle) == PHIPFS_STATUS_OK);
        close_callback_kind = 6U;
        const uint64_t unchanged_size = disk_size;
        const unsigned old_truncates = truncates;
        const unsigned old_appends = appends;
        uint8_t output = 0xa5U;
        size_t count = 99U;
        uint64_t end_position = 99U;
        enum phipfs_status status;
        switch (operation) {
        case 0U: status = ext4_backend_read(callback_handle, &output, 1U, &count); break;
        case 1U: status = ext4_backend_write(callback_handle, (const uint8_t *)"x", 1U, &count); break;
        case 2U: status = ext4_backend_append(callback_handle, (const uint8_t *)"x", 1U, &count); break;
        case 3U: status = ext4_backend_ftruncate(callback_handle, 1U); break;
        default: status = ext4_backend_seek(callback_handle, 0, PHIPFS_SEEK_END, &end_position); break;
        }
        assert(status == PHIPFS_STATUS_STALE_HANDLE && close_callback_kind == 0U);
        assert(disk_size == unchanged_size && truncates == old_truncates && appends == old_appends);
        assert(output == 0xa5U && (operation >= 3U || count == 0U));
        assert(!volume_has_open_handles(PHIPFS_VOLUME_DATA) && opens == closes);
        assert(handle_state(callback_handle, &state) == PHIPFS_STATUS_STALE_HANDLE);
    }
    assert(ext4_backend_directory_open(PHIPFS_VOLUME_DATA, "file", &callback_handle) == PHIPFS_STATUS_OK);
    close_callback_kind = 5U;
    const unsigned snapshots_before_close = freed_snapshots;
    assert(ext4_backend_directory_read(callback_handle, &entry, &present) == PHIPFS_STATUS_OK);
    assert(present && close_callback_kind == 0U);
    assert(live_snapshots == 0U && freed_snapshots == snapshots_before_close + 1U);
    assert(handle_state(callback_handle, &state) == PHIPFS_STATUS_STALE_HANDLE);
    assert(!volume_has_open_handles(PHIPFS_VOLUME_DATA));
    struct phipfs_stat path_metadata;
    assert(ext4_backend_stat_path(PHIPFS_VOLUME_DATA, "file", &path_metadata) == PHIPFS_STATUS_OK);
    assert(path_metadata.mode == 0100640U && path_metadata.uid == 70000U && path_metadata.gid == 90000U);
    assert(path_metadata.links == 3U && path_metadata.atime_seconds == -1 && path_metadata.atime_nanos == 123U);
    assert(path_metadata.mtime_seconds == INT64_C(2147483648) && path_metadata.mtime_nanos == 999999999U);
    assert(path_metadata.ctime_seconds == INT32_MIN && path_metadata.ctime_nanos == 0U);
    lstat_symbolic = true;
    assert(ext4_backend_lstat_path(PHIPFS_VOLUME_DATA, "file", &path_metadata) == PHIPFS_STATUS_OK);
    assert(path_metadata.object_id == 84U && path_metadata.mode == 0120777U && path_metadata.size == 10U);
    stat_refusals = 1U;
    assert(ext4_backend_lstat_path(PHIPFS_VOLUME_DATA, "file", &path_metadata) == PHIPFS_STATUS_IO);
    assert(path_metadata.object_id == 0U && path_metadata.mode == 0U && path_metadata.atime_seconds == 0);
    lstat_symbolic = false;
    assert(opens == closes);
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        permanent_status = attempt == 0U ? PHIPIA_EXT4_STATUS_IO : PHIPIA_EXT4_STATUS_OK;
        const unsigned previous_opens = opens;
        phipfs_handle prepared = 99U;
        assert(ext4_backend_open_options(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ_WRITE,
            PHIPFS_OPEN_CREATE | PHIPFS_OPEN_TRUNCATE, 01720U, &prepared, &path_metadata) ==
            (attempt == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK));
        assert(prepared_flags == (PHIPFS_OPEN_CREATE | PHIPFS_OPEN_TRUNCATE) && prepared_mode == 01720U);
        assert(opens == previous_opens + 1U && opens == closes);
        if (attempt == 0U) assert(prepared == 0U && path_metadata.object_id == 0U);
        else {
            assert(path_metadata.size == 0U && path_metadata.object_id == 42U);
            assert(ext4_backend_close(prepared) == PHIPFS_STATUS_OK);
        }
    }
    const unsigned before_invalid_open = prepared_opens;
    assert(ext4_backend_open_options(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ,
        PHIPFS_OPEN_TRUNCATE, 0600U, &first, &path_metadata) == PHIPFS_STATUS_ACCESS);
    assert(first == 0U && prepared_opens == before_invalid_open && opens == closes);
    permanent_status = PHIPIA_EXT4_STATUS_EXISTS;
    assert(ext4_backend_open_options(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ_WRITE,
        PHIPFS_OPEN_CREATE | PHIPFS_OPEN_EXCLUSIVE, 0600U, &first, &path_metadata) == PHIPFS_STATUS_EXISTS);
    assert(first == 0U && prepared_flags == (PHIPFS_OPEN_CREATE | PHIPFS_OPEN_EXCLUSIVE));
    assert(path_metadata.object_id == 0U && opens == closes);
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        permanent_status = attempt == 0U ? PHIPIA_EXT4_STATUS_IO : PHIPIA_EXT4_STATUS_OK;
        assert(ext4_backend_mkdir_mode(PHIPFS_VOLUME_DATA, "file", 01720U) ==
            (attempt == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK));
        assert(directory_mode == 01720U && opens == closes);
    }
    assert(ext4_backend_mkdir_mode(PHIPFS_VOLUME_DATA, "file", 0U) == PHIPFS_STATUS_OK);
    assert(directory_mode == 0U);
    assert(ext4_backend_create_directory_probe(PHIPFS_VOLUME_DATA, "file") == PHIPFS_STATUS_OK);
    assert(directory_mode == 0755U);
    const unsigned before_invalid_mkdir = opens;
    assert(ext4_backend_mkdir_mode(PHIPFS_VOLUME_DATA, "file", 010000U) == PHIPFS_STATUS_INVALID_ARGUMENT);
    assert(opens == before_invalid_mkdir && opens == closes);
    assert(ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ, &first) == PHIPFS_STATUS_OK);
    assert(ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ_WRITE, &second) == PHIPFS_STATUS_OK);
    assert(ext4_backend_seek(first, 3, PHIPFS_SEEK_START, &position) == PHIPFS_STATUS_OK);
    pending = true;
    pending_size = 321U;
    sync_refusals = 1U;
    assert(ext4_backend_fsync(first) == PHIPFS_STATUS_IO);
    assert(pending && last_sync_open_count == 2U && opens == closes);
    assert(ext4_backend_fsync(first) == PHIPFS_STATUS_OK);
    assert(!pending && disk_size == 321U && last_sync_open_count == 2U && opens == closes);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 321U && state->offset == 3U);
    assert(handle_state(second, &state) == PHIPFS_STATUS_OK && state->size == 321U);
    inode_links = 0U; // An unlinked inode is still held by both descriptors.
    assert(ext4_backend_fstat(first, &path_metadata) == PHIPFS_STATUS_OK);
    assert(path_metadata.object_id == 42U && path_metadata.links == 0U && path_metadata.size == 321U);
    assert(path_metadata.atime_seconds == -1 && path_metadata.uid == 70000U);
    stat_refusals = 1U;
    assert(ext4_backend_fstat(first, &path_metadata) == PHIPFS_STATUS_IO);
    assert(path_metadata.object_id == 0U && path_metadata.size == 0U);
    inode_links = 3U;
    assert(ext4_backend_publish_file(first, "file", "moved") == PHIPFS_STATUS_ACCESS);
    assert(publication_calls == 0U);
    permanent_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_publish_file(second, "file", "moved") == PHIPFS_STATUS_IO);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_publish_file(second, "file", "moved") == PHIPFS_STATUS_OK);
    assert(publication_calls == 2U && opens == closes);
    assert(ext4_backend_unlink_held_file(first, "file") == PHIPFS_STATUS_ACCESS);
    assert(held_unlink_calls == 0U);
    permanent_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_unlink_held_file(second, "file") == PHIPFS_STATUS_IO);
    permanent_status = PHIPIA_EXT4_STATUS_STALE;
    assert(ext4_backend_unlink_held_file(second, "file") == PHIPFS_STATUS_STALE_HANDLE);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_unlink_held_file(second, "file") == PHIPFS_STATUS_OK);
    assert(held_unlink_calls == 3U && opens == closes);
    callback_handle = first;
    close_callback_kind = 3U;
    const unsigned sync_before_stale = file_sync_calls;
    assert(ext4_backend_fsync(first) == PHIPFS_STATUS_STALE_HANDLE);
    assert(close_callback_kind == 0U && file_sync_calls == sync_before_stale && opens == closes);
    assert(ext4_backend_close(second) == PHIPFS_STATUS_OK);
    assert(ext4_backend_fsync(second) == PHIPFS_STATUS_STALE_HANDLE);
    assert(ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ, &first) == PHIPFS_STATUS_OK);
    callback_handle = first;
    close_callback_kind = 3U;
    assert(ext4_backend_fstat(first, &path_metadata) == PHIPFS_STATUS_STALE_HANDLE);
    assert(path_metadata.object_id == 0U && path_metadata.mode == 0U && opens == closes);
    assert(!volume_has_open_handles(PHIPFS_VOLUME_DATA));
    assert(ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ_WRITE, &first) == PHIPFS_STATUS_OK);
    callback_handle = first;
    close_callback_kind = 3U;
    assert(ext4_backend_unlink_held_file(first, "file") == PHIPFS_STATUS_STALE_HANDLE);
    assert(held_unlink_calls == 3U && opens == closes);
    unmount_refusals = 1U;
    assert(ext4_backend_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_CORRUPT);
    assert(live_mounts == 1U && !ext4_mounts[PHIPFS_VOLUME_DATA].detaching);
    assert(ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ, &first) == PHIPFS_STATUS_OK);
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
