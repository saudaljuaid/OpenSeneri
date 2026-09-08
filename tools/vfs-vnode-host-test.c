/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise shared inode identity and lifetime under actual host contention. */
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#include "../src/kernel/vfs.c"

#define WORKERS 16U
#define ROUNDS 2000U
static _Thread_local bool host_interrupts_enabled = true;
bool cpu_interrupts_enabled(void) { return host_interrupts_enabled; }
void cpu_interrupt_disable(void) { host_interrupts_enabled = false; }
void cpu_interrupt_enable(void) { host_interrupts_enabled = true; }
static unsigned ready;
static bool proceed;
static size_t initial_slots[WORKERS];

static void yield_worker(void)
{
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static void exercise_vnodes(size_t worker)
{
    struct phipfs_stat metadata = { .object_id = 500U };
    assert(vnode_retain(PHIPFS_VOLUME_DATA, "shared", &metadata, &initial_slots[worker]) == PHIPFS_STATUS_OK);
    const uint64_t held_generation = vnode_snapshot(initial_slots[worker]).generation;
    __atomic_fetch_add(&ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&proceed, __ATOMIC_ACQUIRE)) yield_worker();
    vnode_release(initial_slots[worker], held_generation);
    for (size_t round = 0U; round < ROUNDS; ++round) {
        const bool interrupts = (round % 2U) == 0U;
        host_interrupts_enabled = interrupts;
        metadata.object_id = round % 2U == 0U ? 500U + worker % 4U : 10000U + worker;
        metadata.size = worker * ROUNDS + round;
        metadata.mtime_seconds = (int64_t)metadata.size;
        metadata.uid = (uint32_t)metadata.size;
        size_t slot;
        assert(vnode_retain(PHIPFS_VOLUME_DATA, "shared-or-unique", &metadata, &slot) == PHIPFS_STATUS_OK);
        assert(host_interrupts_enabled == interrupts);
        yield_worker();
        struct vfs_vnode_state snapshot = vnode_snapshot(slot);
        assert(host_interrupts_enabled == interrupts && snapshot.active && snapshot.references != 0U);
        assert(snapshot.stat.object_id == metadata.object_id && snapshot.mount_generation == 17U);
        assert(snapshot.stat.size == (uint64_t)snapshot.stat.mtime_seconds && snapshot.stat.size == snapshot.stat.uid);
        if (round % 31U == 0U) {
            const size_t reserved = vnode_reserve();
            assert(reserved < VFS_MAX_VNODES && reserved != slot);
            struct phipfs_stat created = { .object_id = 100000U + worker };
            size_t extra;
            assert(vnode_retain_reserved(PHIPFS_VOLUME_DATA, "reserved", &created, &extra, reserved) == PHIPFS_STATUS_OK);
            vnode_unreserve(reserved);
            assert(host_interrupts_enabled == interrupts);
            vnode_release(extra, vnode_snapshot(extra).generation);
        }
        vnode_release(slot, snapshot.generation);
        assert(host_interrupts_enabled == interrupts);
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID argument)
{
    exercise_vnodes((size_t)(uintptr_t)argument);
    return 0U;
}
#else
static void *worker_main(void *argument)
{
    exercise_vnodes((size_t)(uintptr_t)argument);
    return NULL;
}
#endif

int main(void)
{
    mounts[PHIPFS_VOLUME_DATA].active = true;
    mounts[PHIPFS_VOLUME_DATA].generation = 17U;
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) vnode_buckets[index] = VFS_NO_INDEX;
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
    for (size_t index = 0U; index < WORKERS; ++index) assert(initial_slots[index] == initial_slots[0]);
    assert(vnode_snapshot(initial_slots[0]).references == WORKERS);
    assert(__atomic_load_n(&mounts[PHIPFS_VOLUME_DATA].references, __ATOMIC_ACQUIRE) == 1U);
    __atomic_store_n(&proceed, true, __ATOMIC_RELEASE);
    for (size_t index = 0U; index < WORKERS; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(workers[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(workers[index]));
#else
        assert(pthread_join(workers[index], NULL) == 0);
#endif
    }
    assert(vnode_resources_released() && mounts[PHIPFS_VOLUME_DATA].references == 0U);
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) assert(vnode_buckets[index] == VFS_NO_INDEX);
    struct phipfs_stat metadata = { .object_id = 500U };
    size_t slot;
    assert(vnode_retain(PHIPFS_VOLUME_DATA, "overflow", &metadata, &slot) == PHIPFS_STATUS_OK);
    const uint64_t old_generation = vnode_snapshot(slot).generation;
    vnodes[slot].references = SIZE_MAX;
    size_t refused = VFS_NO_INDEX;
    assert(vnode_retain(PHIPFS_VOLUME_DATA, "overflow", &metadata, &refused) == PHIPFS_STATUS_BUSY && refused == VFS_NO_INDEX);
    vnodes[slot].references = 1U;
    vnode_release(slot, old_generation);
    assert(vnode_retain(PHIPFS_VOLUME_DATA, "reused", &metadata, &slot) == PHIPFS_STATUS_OK);
    vnode_release(slot, old_generation);
    assert(vnode_snapshot(slot).references == 1U);
    vnode_release(slot, vnode_snapshot(slot).generation);
    mounts[PHIPFS_VOLUME_DATA].references = SIZE_MAX;
    assert(vnode_retain(PHIPFS_VOLUME_DATA, "mount-full", &metadata, &slot) == PHIPFS_STATUS_NO_HANDLES);
    assert(!mount_retain(PHIPFS_VOLUME_DATA));
    mounts[PHIPFS_VOLUME_DATA].references = 0U;
    assert(vnode_resources_released() && host_interrupts_enabled);
    puts("VFS concurrent vnode deduplication, coherent snapshots, reservations, reference overflow and retirement PASS");
    return 0;
}
