/* SPDX-License-Identifier: GPL-3.0-only */
/* Concurrent readers of one public directory snapshot consume each entry once. */
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
#define ROUNDS 32U
static _Thread_local bool host_interrupts_enabled = true;
bool cpu_interrupts_enabled(void) { return host_interrupts_enabled; }
void cpu_interrupt_disable(void) { host_interrupts_enabled = false; }
void cpu_interrupt_enable(void) { host_interrupts_enabled = true; }
static unsigned ready;
static bool proceed;
static unsigned seen[PHIPFS_MAX_LIST_ENTRIES];
static phipfs_directory_handle shared;

static void yield_worker(void)
{
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static enum phipfs_status directory_stat(enum phipfs_volume volume, const char *path,
    struct phipfs_stat *stat)
{
    (void)volume; (void)path;
    assert(host_interrupts_enabled);
    *stat = (struct phipfs_stat){ .object_id = 100U, .directory = true };
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status directory_list(enum phipfs_volume volume, const char *path,
    struct phipfs_list_entry *entries, size_t capacity, size_t *count)
{
    (void)volume; (void)path;
    assert(host_interrupts_enabled && capacity == PHIPFS_MAX_LIST_ENTRIES);
    for (size_t index = 0U; index < capacity; ++index) {
        zero_bytes(&entries[index], sizeof(entries[index]));
        entries[index].size = index;
    }
    *count = capacity;
    return PHIPFS_STATUS_OK;
}

static void read_shared_directory(void)
{
    __atomic_fetch_add(&ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&proceed, __ATOMIC_ACQUIRE)) yield_worker();
    for (;;) {
        struct phipfs_list_entry entry;
        bool present;
        assert(phipfs_directory_read(shared, &entry, &present) == PHIPFS_STATUS_OK);
        assert(host_interrupts_enabled);
        if (!present) break;
        assert(entry.size < PHIPFS_MAX_LIST_ENTRIES);
        assert(__atomic_fetch_add(&seen[entry.size], 1U, __ATOMIC_RELAXED) == 0U);
        yield_worker();
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID argument)
{
    (void)argument;
    read_shared_directory();
    return 0U;
}
#else
static void *worker_main(void *argument)
{
    (void)argument;
    read_shared_directory();
    return NULL;
}
#endif

int main(void)
{
    static const struct vfs_backend_ops backend = {
        .stat_path = directory_stat, .list = directory_list, .case_sensitive = true,
    };
    mounts[PHIPFS_VOLUME_DATA].active = true;
    mounts[PHIPFS_VOLUME_DATA].generation = 17U;
    mounts[PHIPFS_VOLUME_DATA].backend = &backend;
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) vnode_buckets[index] = VFS_NO_INDEX;
    for (unsigned round = 0U; round < ROUNDS; ++round) {
        ready = 0U;
        proceed = false;
        zero_bytes(seen, sizeof(seen));
        assert(phipfs_directory_open(PHIPFS_VOLUME_DATA, "shared", &shared) == PHIPFS_STATUS_OK);
#ifdef _WIN32
        HANDLE workers[WORKERS];
#else
        pthread_t workers[WORKERS];
#endif
        for (size_t index = 0U; index < WORKERS; ++index) {
#ifdef _WIN32
            workers[index] = CreateThread(NULL, 0U, worker_main, NULL, 0U, NULL);
            assert(workers[index] != NULL);
#else
            assert(pthread_create(&workers[index], NULL, worker_main, NULL) == 0);
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
        for (size_t index = 0U; index < PHIPFS_MAX_LIST_ENTRIES; ++index) assert(seen[index] == 1U);
        assert(phipfs_directory_close(shared) == PHIPFS_STATUS_OK);
        struct phipfs_list_entry absent;
        bool present = true;
        assert(phipfs_directory_read(shared, &absent, &present) == PHIPFS_STATUS_STALE_HANDLE && !present);
        assert(vnode_resources_released() && mounts[PHIPFS_VOLUME_DATA].references == 0U);
        for (size_t index = 0U; index < VFS_MAX_DIRECTORY_ITERATORS; ++index)
            assert(!directory_claims[index] && !directories[index].active && !directories[index].opening);
    }
    puts("VFS directory snapshot: 16 competing readers, 32 generations, exact once-only entries and empty census PASS");
    return 0;
}
