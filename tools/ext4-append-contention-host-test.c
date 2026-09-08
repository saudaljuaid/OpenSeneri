/* SPDX-License-Identifier: GPL-3.0-only */
/* Production volume exclusion with stable held handles, not a full SMP registry test. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#include "../src/kernel/ext4_fs.c"
static _Thread_local bool host_interrupts_enabled = true;
bool cpu_interrupts_enabled(void) { return host_interrupts_enabled; }
void cpu_interrupt_disable(void) { host_interrupts_enabled = false; }
void cpu_interrupt_enable(void) { host_interrupts_enabled = true; }

#define WORKERS 16U
#define ROUNDS 500U
static unsigned ready;
static bool proceed;
static unsigned owners;
static unsigned busy;
static unsigned opens;
static unsigned closes;
static uint64_t disk_size = 4500U;
static unsigned seen[WORKERS * ROUNDS];
static phipfs_handle handles[WORKERS];
static _Thread_local uint64_t last_end;

static void yield_worker(void)
{
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

enum nvme_status nvme_volume_open(struct nvme_volume_session *session,
    uint32_t controller, bool writable)
{
    assert(controller == 1U && writable && !session->active);
    assert(cpu_interrupts_enabled());
    assert(__atomic_fetch_add(&owners, 1U, __ATOMIC_ACQ_REL) == 0U);
    ++opens;
    session->active = true;
    session->writable = true;
    session->logical_block_bytes = 4096U;
    session->namespace_blocks = 32768U;
    yield_worker();
    return NVME_STATUS_OK;
}

enum nvme_status nvme_volume_close(struct nvme_volume_session *session)
{
    assert(session->active && __atomic_load_n(&owners, __ATOMIC_ACQUIRE) == 1U);
    assert(cpu_interrupts_enabled());
    session->active = false;
    ++closes;
    assert(__atomic_fetch_sub(&owners, 1U, __ATOMIC_ACQ_REL) == 1U);
    return NVME_STATUS_OK;
}

int32_t phipia_ext4_free_bytes(uintptr_t mounted, uint64_t *bytes)
{
    assert(mounted == 1U && __atomic_load_n(&owners, __ATOMIC_ACQUIRE) == 1U);
    *bytes = UINT64_C(32768) * 4096U - disk_size;
    return PHIPIA_EXT4_STATUS_OK;
}

void phipia_ext4_snapshot_free(uintptr_t snapshot)
{
    (void)snapshot;
    assert(!"regular append handles must not own snapshots");
}

int32_t phipia_ext4_append_inode(uintptr_t mounted, uint64_t inode,
    const uint8_t *source, size_t length, uint64_t maximum_size,
    uint64_t *start, size_t *count)
{
    assert(mounted == 1U && inode == 42U && length == sizeof(uint64_t));
    assert(maximum_size == PHIPIA_EXT4_MAX_MUTABLE_FILE_BYTES);
    assert(__atomic_load_n(&owners, __ATOMIC_ACQUIRE) == 1U);
    uint64_t request;
    memcpy(&request, source, sizeof(request));
    assert(request < WORKERS * ROUNDS && seen[request]++ == 0U);
    *start = disk_size;
    *count = length;
    disk_size += length;
    last_end = disk_size;
    yield_worker();
    return PHIPIA_EXT4_STATUS_OK;
}

static void append_records(size_t worker)
{
    __atomic_fetch_add(&ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&proceed, __ATOMIC_ACQUIRE)) yield_worker();
    for (size_t round = 0U; round < ROUNDS; ++round) {
        const uint64_t request = worker * ROUNDS + round;
        enum phipfs_status status;
        size_t count;
        do {
            count = 99U;
            status = ext4_backend_append(handles[worker], (const uint8_t *)&request, sizeof(request), &count);
            if (status == PHIPFS_STATUS_BUSY) {
                assert(count == 0U);
                __atomic_fetch_add(&busy, 1U, __ATOMIC_RELAXED);
                yield_worker();
            }
        } while (status == PHIPFS_STATUS_BUSY);
        assert(status == PHIPFS_STATUS_OK && count == sizeof(request));
        /* Other handles can advance shared EOF, but only this worker owns this cursor. */
        const size_t slot = (size_t)((handles[worker] & UINT64_C(0xff)) - 1U);
        assert(ext4_handles[slot].offset == last_end);
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID argument)
{
    append_records((size_t)(uintptr_t)argument);
    return 0U;
}
#else
static void *worker_main(void *argument)
{
    append_records((size_t)(uintptr_t)argument);
    return NULL;
}
#endif

int main(void)
{
    struct ext4_mount_state *mount = &ext4_mounts[PHIPFS_VOLUME_DATA];
    mount->active = mount->healthy = true;
    mount->generation = mount->rust_mount = 1U;
    mount->controller_index = 1U;
    mount->admitted_media_bytes = UINT64_C(32768) * 4096U;
    for (size_t index = 0U; index < WORKERS; ++index)
        assert(allocate_handle(PHIPFS_VOLUME_DATA, "shared", 42U, disk_size,
            PHIPFS_ACCESS_READ_WRITE, false, 0U, &handles[index]) == PHIPFS_STATUS_OK);
#ifdef _WIN32
    HANDLE workers[WORKERS];
#else
    pthread_t workers[WORKERS];
#endif
    for (size_t index = 0U; index < WORKERS; ++index) {
#ifdef _WIN32
        workers[index] = CreateThread(NULL, 0U, worker_main, (void *)(uintptr_t)index, 0U, NULL);
        assert(workers[index] != NULL);
#else
        assert(pthread_create(&workers[index], NULL, worker_main, (void *)(uintptr_t)index) == 0);
#endif
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != WORKERS) yield_worker();
    __atomic_store_n(&proceed, true, __ATOMIC_RELEASE);
    for (size_t index = 0U; index < WORKERS; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(workers[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(workers[index]));
#else
        assert(pthread_join(workers[index], NULL) == 0);
#endif
    }
    assert(disk_size == 4500U + WORKERS * ROUNDS * sizeof(uint64_t));
    assert(opens == WORKERS * ROUNDS && closes == opens && busy != 0U && owners == 0U);
    assert(!mount->operation_active && !mount->session.active && !mount->close_failed);
    assert(mount->completion_count == WORKERS * ROUNDS);
    for (size_t index = 0U; index < WORKERS * ROUNDS; ++index) assert(seen[index] == 1U);
    for (size_t index = 0U; index < WORKERS; ++index) {
        const size_t slot = (size_t)((handles[index] & UINT64_C(0xff)) - 1U);
        assert(ext4_handles[slot].size == disk_size);
        retire_handle_slot(slot);
    }
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index)
        assert(!ext4_handles[index].active && !ext4_handle_claims[index]);
    puts("ext4 stable held append handles: 16 writers, 8000 exact records, BUSY exclusion, coherent EOF/cursors and balanced leases PASS");
    return 0;
}
