/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Focused inode-fsync contract test.
 *
 * This includes the production ext4 backend and supplies only the platform
 * callbacks at its documented Rust/NVMe boundary. The callback fixture keeps a
 * durable byte image separate from the pending image, allowing every refusal
 * case to assert that fsync did not report success or publish data early.
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

enum fsync_failure_boundary {
    FSYNC_FAILURE_NONE,
    FSYNC_FAILURE_ORDERED_DATA,
    FSYNC_FAILURE_JOURNAL_PAYLOAD,
    FSYNC_FAILURE_COMMIT,
    FSYNC_FAILURE_CHECKPOINT,
    FSYNC_FAILURE_JOURNAL_STATE,
    FSYNC_FAILURE_FLUSH_COMPLETION,
};

static enum fsync_failure_boundary failure_boundary;
static bool open_failure;
static bool close_failure;
static bool stat_failure;
static bool pending;
static uint8_t durable_bytes[64];
static uint8_t pending_bytes[64];
static size_t durable_length;
static size_t pending_length;
static uint64_t durable_size;
static uint64_t pending_size;
static uint64_t requested_inode;
static unsigned fsync_calls;
static unsigned sync_calls;
static unsigned stat_calls;
static unsigned opens;
static unsigned closes;

static void clear_fixture(void)
{
    memset(ext4_mounts, 0, sizeof(ext4_mounts));
    memset(ext4_handles, 0, sizeof(ext4_handles));
    memset(ext4_handle_claims, 0, sizeof(ext4_handle_claims));
    memset(durable_bytes, 0, sizeof(durable_bytes));
    memset(pending_bytes, 0, sizeof(pending_bytes));
    ext4_mounts[PHIPFS_VOLUME_DATA].active = true;
    ext4_mounts[PHIPFS_VOLUME_DATA].healthy = true;
    ext4_mounts[PHIPFS_VOLUME_DATA].rust_mount = 1U;
    ext4_mounts[PHIPFS_VOLUME_DATA].generation = 700U;
    ext4_mounts[PHIPFS_VOLUME_DATA].controller_index = 1U;
    ext4_mounts[PHIPFS_VOLUME_DATA].media_bytes = 32768U * 4096U;
    ext4_mounts[PHIPFS_VOLUME_DATA].admitted_media_bytes =
        ext4_mounts[PHIPFS_VOLUME_DATA].media_bytes;
    durable_length = 12U;
    memcpy(durable_bytes, "initial data", durable_length);
    durable_size = durable_length;
    pending = false;
    pending_length = 0U;
    pending_size = durable_size;
    requested_inode = 42U;
    failure_boundary = FSYNC_FAILURE_NONE;
    open_failure = false;
    close_failure = false;
    stat_failure = false;
    fsync_calls = 0U;
    sync_calls = 0U;
    stat_calls = 0U;
    opens = 0U;
    closes = 0U;
}

static void stage_bytes(const char *bytes)
{
    pending_length = strlen(bytes);
    assert(pending_length <= sizeof(pending_bytes));
    memcpy(pending_bytes, bytes, pending_length);
    pending_size = pending_length;
    pending = true;
}

static void assert_durable_bytes(const char *expected)
{
    const size_t length = strlen(expected);
    assert(durable_length == length);
    assert(memcmp(durable_bytes, expected, length) == 0);
}

enum nvme_status nvme_volume_open(struct nvme_volume_session *session,
    uint32_t controller_index, bool writable)
{
    assert(session != NULL && !session->active);
    assert(controller_index == 1U && writable);
    if (open_failure) return NVME_STATUS_TEARDOWN_FAILURE;
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
    if (close_failure) {
        session->state = NVME_FILESYSTEM_SESSION_STOPPING;
        session->close_teardown_status = NVME_STATUS_TEARDOWN_FAILURE;
        return NVME_STATUS_TEARDOWN_FAILURE;
    }
    session->active = false;
    ++closes;
    return NVME_STATUS_OK;
}

int32_t phipia_ext4_free_bytes(uintptr_t mounted, uint64_t *bytes)
{
    assert(mounted == 1U && bytes != NULL);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    *bytes = 123U * 4096U;
    return PHIPIA_EXT4_STATUS_OK;
}

void phipia_ext4_snapshot_free(uintptr_t snapshot)
{
    assert(snapshot == 0U);
}

int32_t phipia_ext4_sync(uintptr_t mounted, const uint64_t *open_inodes,
    size_t open_count)
{
    ++sync_calls;
    assert(mounted == 1U && open_inodes != NULL && open_count <= EXT4_MAX_HANDLES);
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_stat_inode(uintptr_t mounted, uint64_t inode,
    struct phipia_ext4_metadata *metadata)
{
    ++stat_calls;
    assert(mounted == 1U && metadata != NULL);
    if (stat_failure) return PHIPIA_EXT4_STATUS_IO;
    memset(metadata, 0, sizeof(*metadata));
    metadata->inode = inode;
    metadata->size = inode == 42U ? durable_size : 99U;
    metadata->file_type = PHIPIA_EXT4_FILE_REGULAR;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_fsync(uintptr_t mounted, uint64_t inode)
{
    ++fsync_calls;
    assert(mounted == 1U && inode != 0U);
    if (inode != requested_inode) return PHIPIA_EXT4_STATUS_BUSY;
    if (failure_boundary != FSYNC_FAILURE_NONE) return PHIPIA_EXT4_STATUS_IO;
    if (pending) {
        memcpy(durable_bytes, pending_bytes, pending_length);
        durable_length = pending_length;
        durable_size = pending_size;
        pending = false;
    }
    return PHIPIA_EXT4_STATUS_OK;
}

static phipfs_handle open_file(uint64_t inode, enum phipfs_access access)
{
    phipfs_handle handle = 0U;
    assert(allocate_handle(PHIPFS_VOLUME_DATA, inode == 42U ? "file" : "other",
        inode, durable_size, access, false, 0U, &handle) == PHIPFS_STATUS_OK);
    return handle;
}

static phipfs_handle open_directory(void)
{
    phipfs_handle handle = 0U;
    assert(allocate_handle(PHIPFS_VOLUME_DATA, "directory", 7U, 0U,
        PHIPFS_ACCESS_READ, true, 0U, &handle) == PHIPFS_STATUS_OK);
    return handle;
}

static void close_file(phipfs_handle handle)
{
    assert(ext4_backend_close(handle) == PHIPFS_STATUS_OK);
}

static void assert_clean_operation_counts(unsigned old_fsync,
    uint64_t old_completion)
{
    assert(fsync_calls == old_fsync);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].completion_count == old_completion);
    assert(opens == closes);
}

static void test_clean_idempotence_and_shared_inode(void)
{
    phipfs_handle first;
    phipfs_handle second;
    const uint64_t before = ext4_mounts[PHIPFS_VOLUME_DATA].completion_count;

    first = open_file(42U, PHIPFS_ACCESS_READ);
    second = open_file(42U, PHIPFS_ACCESS_READ_WRITE);
    assert(ext4_backend_fsync(first) == PHIPFS_STATUS_OK);
    assert(ext4_backend_fsync(second) == PHIPFS_STATUS_OK);
    assert(ext4_backend_fsync(second) == PHIPFS_STATUS_OK);
    assert(fsync_calls == 3U);
    assert(sync_calls == 0U);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].completion_count == before + 3U);
    assert_durable_bytes("initial data");
    assert(ext4_handles[(first & 0xffU) - 1U].size == durable_size);
    assert(ext4_handles[(second & 0xffU) - 1U].size == durable_size);
    close_file(first);
    close_file(second);
    assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    assert(sync_calls == 1U);
}

static void test_each_durability_boundary_retries(void)
{
    static const enum fsync_failure_boundary boundaries[] = {
        FSYNC_FAILURE_ORDERED_DATA,
        FSYNC_FAILURE_JOURNAL_PAYLOAD,
        FSYNC_FAILURE_COMMIT,
        FSYNC_FAILURE_CHECKPOINT,
        FSYNC_FAILURE_JOURNAL_STATE,
        FSYNC_FAILURE_FLUSH_COMPLETION,
    };
    static const char *const payloads[] = {
        "ordered retry", "journal retry", "commit retry", "checkpoint retry",
        "tail retry", "flush retry",
    };
    phipfs_handle first = open_file(42U, PHIPFS_ACCESS_READ_WRITE);
    phipfs_handle second = open_file(42U, PHIPFS_ACCESS_READ_WRITE);

    for (size_t index = 0U; index < sizeof(boundaries) / sizeof(boundaries[0]); ++index) {
        const unsigned old_fsync = fsync_calls;
        const uint64_t old_completion = ext4_mounts[PHIPFS_VOLUME_DATA].completion_count;
        const size_t old_length = durable_length;
        uint8_t old_bytes[sizeof(durable_bytes)];
        memcpy(old_bytes, durable_bytes, sizeof(old_bytes));
        stage_bytes(payloads[index]);
        failure_boundary = boundaries[index];
        assert(ext4_backend_fsync(first) == PHIPFS_STATUS_IO);
        assert(pending && (durable_length != pending_length ||
            memcmp(durable_bytes, pending_bytes, durable_length < pending_length ?
                durable_length : pending_length) != 0));
        assert(durable_length == old_length);
        assert(memcmp(durable_bytes, old_bytes, old_length) == 0);
        assert_clean_operation_counts(old_fsync + 1U, old_completion);
        failure_boundary = FSYNC_FAILURE_NONE;
        assert(ext4_backend_fsync(second) == PHIPFS_STATUS_OK);
        assert(!pending);
        assert_durable_bytes(payloads[index]);
        assert(ext4_mounts[PHIPFS_VOLUME_DATA].completion_count == old_completion + 1U);
        assert(ext4_handles[(first & 0xffU) - 1U].size == pending_size);
        assert(ext4_handles[(second & 0xffU) - 1U].size == pending_size);
    }
    close_file(first);
    close_file(second);
}

static void test_unrelated_inode_and_statuses(void)
{
    phipfs_handle file = open_file(42U, PHIPFS_ACCESS_READ_WRITE);
    phipfs_handle other = open_file(84U, PHIPFS_ACCESS_READ_WRITE);
    phipfs_handle directory = open_directory();
    phipfs_handle wrong_volume = 0U;
    const unsigned old_fsync = fsync_calls;
    const uint64_t old_completion = ext4_mounts[PHIPFS_VOLUME_DATA].completion_count;

    assert(allocate_handle(PHIPFS_VOLUME_SYSTEM, "system-file", 42U, durable_size,
        PHIPFS_ACCESS_READ_WRITE, false, 0U, &wrong_volume) == PHIPFS_STATUS_OK);
    assert(ext4_backend_fsync(wrong_volume) == PHIPFS_STATUS_STALE_HANDLE);
    assert(fsync_calls == old_fsync);
    assert(ext4_handles[(file & 0xffU) - 1U].generation != 0U);
    assert(ext4_backend_fsync(file ^ UINT64_C(0x100)) == PHIPFS_STATUS_STALE_HANDLE);
    assert(fsync_calls == old_fsync);

    stage_bytes("file only");
    assert(ext4_backend_fsync(other) == PHIPFS_STATUS_BUSY);
    assert(pending);
    assert_clean_operation_counts(old_fsync + 1U, old_completion);
    assert(ext4_backend_fsync(directory) == PHIPFS_STATUS_IS_DIRECTORY);
    assert_clean_operation_counts(old_fsync + 1U, old_completion);
    assert(ext4_backend_fsync(file) == PHIPFS_STATUS_OK);
    assert(!pending);
    assert_durable_bytes("file only");
    close_file(other);
    close_file(file);
    assert(ext4_backend_close(wrong_volume) == PHIPFS_STATUS_STALE_HANDLE);
    assert(ext4_backend_fsync(file) == PHIPFS_STATUS_STALE_HANDLE);
    assert(ext4_backend_fsync(UINT64_C(0x1234)) == PHIPFS_STATUS_STALE_HANDLE);
}

static void test_storage_open_close_and_reload_retry(void)
{
    phipfs_handle file = open_file(42U, PHIPFS_ACCESS_READ_WRITE);
    const uint64_t before = ext4_mounts[PHIPFS_VOLUME_DATA].completion_count;
    const unsigned old_fsync = fsync_calls;
    stage_bytes("open refusal");
    open_failure = true;
    assert(ext4_backend_fsync(file) == PHIPFS_STATUS_IO);
    assert(pending && fsync_calls == old_fsync);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].completion_count == before);
    open_failure = false;
    assert(ext4_backend_fsync(file) == PHIPFS_STATUS_OK);
    assert(!pending && fsync_calls == old_fsync + 1U);

    stage_bytes("close refusal");
    close_failure = true;
    assert(ext4_backend_fsync(file) == PHIPFS_STATUS_IO);
    assert(!pending);
    assert_durable_bytes("close refusal");
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].close_failed);
    close_failure = false;
    assert(ext4_backend_fsync(file) == PHIPFS_STATUS_OK);
    assert(!ext4_mounts[PHIPFS_VOLUME_DATA].close_failed);
    close_file(file);
}

static void test_refresh_failure_does_not_publish_size(void)
{
    phipfs_handle first = open_file(42U, PHIPFS_ACCESS_READ_WRITE);
    phipfs_handle second = open_file(42U, PHIPFS_ACCESS_READ_WRITE);
    const size_t first_slot = (size_t)((first & 0xffU) - 1U);
    const size_t second_slot = (size_t)((second & 0xffU) - 1U);
    const uint64_t old_first_size = ext4_handles[first_slot].size;
    const uint64_t old_second_size = ext4_handles[second_slot].size;
    const uint64_t old_completion = ext4_mounts[PHIPFS_VOLUME_DATA].completion_count;

    stage_bytes("refresh retry");
    stat_failure = true;
    assert(ext4_backend_fsync(first) == PHIPFS_STATUS_IO);
    assert(!pending);
    assert_durable_bytes("refresh retry");
    assert(ext4_handles[first_slot].size == old_first_size);
    assert(ext4_handles[second_slot].size == old_second_size);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].completion_count == old_completion);
    stat_failure = false;
    assert(ext4_backend_fsync(second) == PHIPFS_STATUS_OK);
    assert(ext4_handles[first_slot].size == durable_size);
    assert(ext4_handles[second_slot].size == durable_size);
    close_file(first);
    close_file(second);
}

int main(void)
{
    clear_fixture();
    test_clean_idempotence_and_shared_inode();
    test_each_durability_boundary_retries();
    test_unrelated_inode_and_statuses();
    test_storage_open_close_and_reload_retry();
    test_refresh_failure_does_not_publish_size();
    puts("ext4 inode fsync durability, retries, identities and statuses: PASS");
    return 0;
}
