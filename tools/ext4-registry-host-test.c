/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse the production backend and its existing storage/Rust test doubles. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#define main ext4_existing_host_main
#include "ext4-vfs-host-test.c"
#undef main

#define REGISTRY_WORKERS 16U
static unsigned registry_ready;
static bool registry_proceed;
static bool registry_churn;
static phipfs_handle registry_shared;
static unsigned registry_closed;
static unsigned registry_stale;

static void registry_yield(void)
{
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static void registry_exercise(size_t worker)
{
    __atomic_fetch_add(&registry_ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&registry_proceed, __ATOMIC_ACQUIRE)) registry_yield();
    if (!registry_churn) {
        const enum phipfs_status status = ext4_backend_close(registry_shared);
        assert(status == PHIPFS_STATUS_OK || status == PHIPFS_STATUS_STALE_HANDLE);
        __atomic_fetch_add(status == PHIPFS_STATUS_OK ? &registry_closed : &registry_stale, 1U, __ATOMIC_RELAXED);
        return;
    }
    const enum phipfs_volume volume = (enum phipfs_volume)(worker % PHIPFS_VOLUME_COUNT);
    for (size_t round = 0U; round < 500U; ++round) {
        phipfs_handle handle;
        enum phipfs_status status;
        do {
            status = allocate_handle(volume, "owned", 1000U + worker, round,
                PHIPFS_ACCESS_READ_WRITE, false, 0U, &handle);
            if (status == PHIPFS_STATUS_NO_HANDLES) registry_yield();
        } while (status == PHIPFS_STATUS_NO_HANDLES);
        assert(status == PHIPFS_STATUS_OK);
        registry_yield();
        struct ext4_handle_state snapshot;
        assert(handle_snapshot(handle, &snapshot) == PHIPFS_STATUS_OK);
        assert(snapshot.generation == handle >> 8U && snapshot.mount_generation == 17U);
        assert(snapshot.volume == volume && snapshot.inode == 1000U + worker && snapshot.size == round);
        assert(snapshot.offset == 0U && !snapshot.directory && !snapshot.closing && snapshot.active);
        uint64_t inodes[EXT4_MAX_HANDLES];
        const size_t count = collect_open_inodes(volume, inodes, false);
        bool found = false;
        for (size_t index = 0U; index < count; ++index) {
            assert(inodes[index] >= 1000U && inodes[index] < 1000U + REGISTRY_WORKERS);
            assert((inodes[index] - 1000U) % PHIPFS_VOLUME_COUNT == (uint64_t)volume);
            if (inodes[index] == 1000U + worker) found = true;
        }
        assert(found && volume_has_open_handles(volume));
        assert(ext4_backend_close(handle) == PHIPFS_STATUS_OK);
        assert(ext4_backend_close(handle) == PHIPFS_STATUS_STALE_HANDLE);
        assert(handle_snapshot(handle, &snapshot) == PHIPFS_STATUS_STALE_HANDLE);
        assert(cpu_interrupts_enabled());
    }
}

#ifdef _WIN32
static DWORD WINAPI registry_worker(LPVOID argument)
{
    registry_exercise((size_t)(uintptr_t)argument);
    return 0U;
}
#else
static void *registry_worker(void *argument)
{
    registry_exercise((size_t)(uintptr_t)argument);
    return NULL;
}
#endif

static void run_registry_workers(void)
{
    registry_ready = 0U;
    registry_proceed = false;
#ifdef _WIN32
    HANDLE workers[REGISTRY_WORKERS];
#else
    pthread_t workers[REGISTRY_WORKERS];
#endif
    for (size_t index = 0U; index < REGISTRY_WORKERS; ++index) {
#ifdef _WIN32
        workers[index] = CreateThread(NULL, 0U, registry_worker, (void *)(uintptr_t)index, 0U, NULL);
        assert(workers[index] != NULL);
#else
        assert(pthread_create(&workers[index], NULL, registry_worker, (void *)(uintptr_t)index) == 0);
#endif
    }
    while (__atomic_load_n(&registry_ready, __ATOMIC_ACQUIRE) != REGISTRY_WORKERS) registry_yield();
    __atomic_store_n(&registry_proceed, true, __ATOMIC_RELEASE);
    for (size_t index = 0U; index < REGISTRY_WORKERS; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(workers[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(workers[index]));
#else
        assert(pthread_join(workers[index], NULL) == 0);
#endif
    }
}

int main(void)
{
    assert(ext4_existing_host_main() == 0);
    ext4_backend_initialize();
    for (size_t index = 0U; index < PHIPFS_VOLUME_COUNT; ++index) {
        ext4_mounts[index].active = ext4_mounts[index].healthy = true;
        ext4_mounts[index].generation = 17U;
    }
    for (unsigned round = 0U; round < 32U; ++round) {
        registry_closed = registry_stale = 0U;
        assert(allocate_handle(PHIPFS_VOLUME_DATA, "closing", 42U, 0U,
            PHIPFS_ACCESS_READ, false, 0U, &registry_shared) == PHIPFS_STATUS_OK);
        const bool deferred = round % 2U != 0U;
        if (deferred) assert(reserve_operation(&ext4_mounts[PHIPFS_VOLUME_DATA]) == PHIPFS_STATUS_OK);
        run_registry_workers();
        assert(registry_closed == 1U && registry_stale == REGISTRY_WORKERS - 1U);
        const size_t slot = (size_t)((registry_shared & UINT64_C(0xff)) - 1U);
        if (deferred) {
            assert(ext4_handle_claims[slot] && ext4_handles[slot].closing && ext4_handles[slot].active);
            release_operation(&ext4_mounts[PHIPFS_VOLUME_DATA]);
        }
        assert(!ext4_handle_claims[slot] && !ext4_handles[slot].active);
    }
    registry_churn = true;
    run_registry_workers();
    for (size_t index = 0U; index < PHIPFS_VOLUME_COUNT; ++index) {
        assert(!ext4_mounts[index].operation_active && !volume_has_open_handles((enum phipfs_volume)index));
    }
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index)
        assert(!ext4_handle_claims[index] && !ext4_handles[index].active && !ext4_handles[index].closing);
    puts("ext4 registry: one competing close owner, deferred claims, 8000 cross-volume generations and coherent inode census PASS");
    return 0;
}
