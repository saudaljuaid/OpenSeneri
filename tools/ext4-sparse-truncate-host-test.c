/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Focused sparse/truncate contract test for the production C backend.
 *
 * The callbacks below are a deliberately small Rust/NVMe boundary fixture;
 * they model the durable inode state and verify that failed calls do not
 * publish C handle state or completion state. Sparse allocation and replay
 * are covered by the real coordinator fixture test.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/kernel/ext4_fs.c"

static _Thread_local bool host_interrupts_enabled = true;
bool cpu_interrupts_enabled(void) { return host_interrupts_enabled; }
void cpu_interrupt_disable(void) { host_interrupts_enabled = false; }
void cpu_interrupt_enable(void) { host_interrupts_enabled = true; }

static uint8_t disk_bytes[128];
static uint64_t disk_size;
static int32_t write_status;
static int32_t truncate_status;
static int32_t probe_status;
static int32_t stat_status;
static int32_t fsync_status;
static unsigned opens;
static unsigned closes;
static unsigned write_calls;
static unsigned truncate_calls;
static unsigned probe_calls;
static unsigned stat_calls;
static unsigned fsync_calls;

static void clear_fixture(void)
{
    memset(ext4_mounts, 0, sizeof(ext4_mounts));
    memset(ext4_handles, 0, sizeof(ext4_handles));
    memset(ext4_handle_claims, 0, sizeof(ext4_handle_claims));
    memset(disk_bytes, 0, sizeof(disk_bytes));
    ext4_mounts[PHIPFS_VOLUME_DATA].active = true;
    ext4_mounts[PHIPFS_VOLUME_DATA].healthy = true;
    ext4_mounts[PHIPFS_VOLUME_DATA].rust_mount = 1U;
    ext4_mounts[PHIPFS_VOLUME_DATA].generation = 900U;
    ext4_mounts[PHIPFS_VOLUME_DATA].controller_index = 1U;
    ext4_mounts[PHIPFS_VOLUME_DATA].media_bytes = 32768U * 4096U;
    ext4_mounts[PHIPFS_VOLUME_DATA].admitted_media_bytes =
        ext4_mounts[PHIPFS_VOLUME_DATA].media_bytes;
    disk_size = 0U;
    write_status = PHIPIA_EXT4_STATUS_OK;
    truncate_status = PHIPIA_EXT4_STATUS_OK;
    probe_status = PHIPIA_EXT4_STATUS_OK;
    stat_status = PHIPIA_EXT4_STATUS_OK;
    fsync_status = PHIPIA_EXT4_STATUS_OK;
    opens = 0U;
    closes = 0U;
    write_calls = 0U;
    truncate_calls = 0U;
    probe_calls = 0U;
    stat_calls = 0U;
    fsync_calls = 0U;
}

enum nvme_status nvme_volume_open(struct nvme_volume_session *session,
    uint32_t controller_index, bool writable)
{
    assert(session != NULL && !session->active);
    assert(controller_index == 1U && writable);
    memset(session, 0, sizeof(*session));
    session->generation = (uint64_t)opens + 1U;
    session->namespace_blocks = 32768U;
    session->logical_block_bytes = 4096U;
    session->controller_index = controller_index;
    session->writable = writable;
    session->active = true;
    ++opens;
    return NVME_STATUS_OK;
}

enum nvme_status nvme_volume_close(struct nvme_volume_session *session)
{
    assert(session != NULL && session->active);
    session->active = false;
    ++closes;
    return NVME_STATUS_OK;
}

int32_t phipia_ext4_transaction_probe(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, uint64_t offset, const uint8_t *source, size_t length,
    size_t *written)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(source != NULL && written != NULL &&
        ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    ++probe_calls;
    if (probe_status != PHIPIA_EXT4_STATUS_OK) return probe_status;
    assert(offset <= (uint64_t)sizeof(disk_bytes) &&
        (uint64_t)length <= (uint64_t)sizeof(disk_bytes) - offset);
    const size_t start = (size_t)offset;
    memcpy(&disk_bytes[start], source, length);
    if (offset + length > disk_size) disk_size = offset + length;
    *written = length;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_truncate_probe(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, uint64_t size)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    ++truncate_calls;
    if (truncate_status != PHIPIA_EXT4_STATUS_OK) return truncate_status;
    disk_size = size;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_truncate_inode(uintptr_t mounted, uint64_t inode, uint64_t size)
{
    assert(inode == 42U);
    return phipia_ext4_truncate_probe(mounted, (const uint8_t *)"file", 4U, size);
}

int32_t phipia_ext4_write_inode(uintptr_t mounted, uint64_t inode, uint64_t offset,
    const uint8_t *source, size_t length, size_t *written)
{
    assert(mounted == 1U && inode == 42U && source != NULL && written != NULL &&
        ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    ++write_calls;
    if (write_status != PHIPIA_EXT4_STATUS_OK) return write_status;
    assert(offset <= (uint64_t)sizeof(disk_bytes) &&
        (uint64_t)length <= (uint64_t)sizeof(disk_bytes) - offset);
    const size_t start = (size_t)offset;
    memcpy(&disk_bytes[start], source, length);
    if (offset + length > disk_size) disk_size = offset + length;
    *written = length;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_stat(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, struct phipia_ext4_metadata *metadata)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(metadata != NULL);
    ++stat_calls;
    if (stat_status != PHIPIA_EXT4_STATUS_OK) return stat_status;
    memset(metadata, 0, sizeof(*metadata));
    metadata->inode = 42U;
    metadata->size = disk_size;
    metadata->mode = 0100640U;
    metadata->links = 1U;
    metadata->file_type = PHIPIA_EXT4_FILE_REGULAR;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_stat_inode(uintptr_t mounted, uint64_t inode,
    struct phipia_ext4_metadata *metadata)
{
    assert(inode == 42U);
    return phipia_ext4_stat(mounted, (const uint8_t *)"file", 4U, metadata);
}

int32_t phipia_ext4_fsync(uintptr_t mounted, uint64_t inode)
{
    assert(mounted == 1U && inode == 42U);
    ++fsync_calls;
    return fsync_status;
}

static phipfs_handle open_file(enum phipfs_access access)
{
    phipfs_handle handle = 0U;
    assert(allocate_handle(PHIPFS_VOLUME_DATA, "file", 42U, disk_size,
        access, false, 0U, &handle) == PHIPFS_STATUS_OK);
    return handle;
}

static void test_growth_shrink_shared_state_and_cursor(void)
{
    const uint8_t source[] = { 0x31U, 0x32U, 0x33U, 0x34U };
    const uint64_t grown = 3U * 4096U + 37U;
    const uint64_t shrink = 4096U + 5U;
    const phipfs_handle first = open_file(PHIPFS_ACCESS_READ_WRITE);
    const phipfs_handle second = open_file(PHIPFS_ACCESS_READ_WRITE);
    const size_t first_slot = (size_t)((first & 0xffU) - 1U);
    const size_t second_slot = (size_t)((second & 0xffU) - 1U);
    uint64_t position = 7U;
    const unsigned old_truncates = truncate_calls;
    const uint64_t old_completion = ext4_backend_completion_count(PHIPFS_VOLUME_DATA);
    uint8_t before_growth[sizeof(disk_bytes)] = { 0 };
    memcpy(before_growth, disk_bytes, sizeof(disk_bytes));

    assert(ext4_backend_seek(first, (int64_t)position, PHIPFS_SEEK_START,
        &position) == PHIPFS_STATUS_OK);
    truncate_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_ftruncate(first, grown) == PHIPFS_STATUS_IO);
    assert(ext4_handles[first_slot].size == 0U);
    assert(ext4_handles[second_slot].size == 0U);
    assert(disk_size == 0U && memcmp(disk_bytes, before_growth,
        sizeof(disk_bytes)) == 0);
    assert(ext4_backend_completion_count(PHIPFS_VOLUME_DATA) == old_completion);
    assert(truncate_calls == old_truncates + 1U);
    truncate_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_ftruncate(first, grown) == PHIPFS_STATUS_OK);
    assert(ext4_handles[first_slot].size == grown);
    assert(ext4_handles[second_slot].size == grown);
    assert(ext4_handles[first_slot].offset == 7U);
    assert(truncate_calls == old_truncates + 2U);

    uint8_t guarded[sizeof(source) + 2U];
    guarded[0] = 0xa5U;
    memcpy(&guarded[1], source, sizeof(source));
    guarded[sizeof(guarded) - 1U] = 0xa5U;
    size_t written = 0U;
    assert(ext4_backend_write(first, &guarded[1], sizeof(source), &written) == PHIPFS_STATUS_OK);
    assert(written == sizeof(source));
    assert(guarded[0] == 0xa5U && guarded[sizeof(guarded) - 1U] == 0xa5U);
    assert(ext4_handles[first_slot].offset == 11U);
    assert(ext4_handles[second_slot].size == grown);

    const uint64_t before_size = ext4_handles[first_slot].size;
    const uint64_t before_completion = ext4_backend_completion_count(PHIPFS_VOLUME_DATA);
    uint8_t before_bytes[sizeof(disk_bytes)] = { 0 };
    memcpy(before_bytes, disk_bytes, sizeof(disk_bytes));
    truncate_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_ftruncate(first, shrink) == PHIPFS_STATUS_IO);
    assert(ext4_handles[first_slot].size == before_size);
    assert(ext4_handles[second_slot].size == grown);
    assert(memcmp(disk_bytes, before_bytes, sizeof(disk_bytes)) == 0);
    assert(ext4_backend_completion_count(PHIPFS_VOLUME_DATA) == before_completion);
    truncate_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_ftruncate(second, shrink) == PHIPFS_STATUS_OK);
    assert(ext4_handles[first_slot].size == shrink);
    assert(ext4_handles[second_slot].size == shrink);
    assert(ext4_handles[first_slot].offset == 11U);

    assert(ext4_backend_close(first) == PHIPFS_STATUS_OK);
    assert(ext4_backend_close(second) == PHIPFS_STATUS_OK);
}

static void test_bounds_and_fsync_failure(void)
{
    const uint8_t source[] = { 0x41U, 0x42U };
    const unsigned old_probe = probe_calls;
    const unsigned old_truncate = truncate_calls;
    const unsigned old_fsync = fsync_calls;
    const uint64_t old_completion = ext4_backend_completion_count(PHIPFS_VOLUME_DATA);
    const phipfs_handle handle = open_file(PHIPFS_ACCESS_READ_WRITE);
    size_t written = 99U;

    assert(ext4_backend_transaction_probe(PHIPFS_VOLUME_DATA, "file",
        PHIPIA_EXT4_MAX_MUTABLE_FILE_BYTES - 1U, source, sizeof(source), &written) ==
        PHIPFS_STATUS_RANGE);
    assert(written == 0U && probe_calls == old_probe);
    assert(ext4_backend_truncate_probe(PHIPFS_VOLUME_DATA, "file",
        PHIPIA_EXT4_MAX_MUTABLE_FILE_BYTES + 1U) == PHIPFS_STATUS_RANGE);
    assert(truncate_calls == old_truncate);

    fsync_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_fsync(handle) == PHIPFS_STATUS_IO);
    assert(fsync_calls == old_fsync + 1U);
    assert(ext4_backend_completion_count(PHIPFS_VOLUME_DATA) == old_completion);
    fsync_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_fsync(handle) == PHIPFS_STATUS_OK);
    assert(ext4_backend_completion_count(PHIPFS_VOLUME_DATA) == old_completion + 1U);
    assert(ext4_backend_close(handle) == PHIPFS_STATUS_OK);
}

int main(void)
{
    clear_fixture();
    test_growth_shrink_shared_state_and_cursor();
    test_bounds_and_fsync_failure();
    assert(opens == closes);
    puts("ext4 sparse growth, truncate rollback, bounds and fsync: PASS");
    return 0;
}
