/* SPDX-License-Identifier: GPL-3.0-only */
/* Competing public mount calls must not overlap backend lifecycle callbacks. */
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
static _Thread_local bool host_interrupts_enabled = true;
bool cpu_interrupts_enabled(void) { return host_interrupts_enabled; }
void cpu_interrupt_disable(void) { host_interrupts_enabled = false; }
void cpu_interrupt_enable(void) { host_interrupts_enabled = true; }
static unsigned phase, completed, backend_calls;
static bool backend_entered, release_backend;
static bool lookup_fails;
static bool observer_done;
static unsigned observations;
static enum phipfs_status results[WORKERS];

static void yield_worker(void)
{
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static enum phipfs_status backend_transition(enum phipfs_volume volume)
{
    assert(volume == PHIPFS_VOLUME_DATA && cpu_interrupts_enabled());
    __atomic_fetch_add(&backend_calls, 1U, __ATOMIC_RELAXED);
    const struct vfs_backend_ops *pinned = NULL;
    assert(mount_pin(volume, &pinned) == PHIPFS_STATUS_NOT_MOUNTED && pinned == NULL);
    assert(phipfs_mount(volume) == PHIPFS_STATUS_BUSY && phipfs_unmount(volume) == PHIPFS_STATUS_BUSY);
    __atomic_store_n(&backend_entered, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_backend, __ATOMIC_ACQUIRE)) yield_worker();
    return phase % 2U == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
}

static struct phipfs_drive_info backend_drive(enum phipfs_volume volume)
{
    assert(volume == PHIPFS_VOLUME_DATA && cpu_interrupts_enabled());
    (void)phipfs_has_atomic_replace(volume);
    return (struct phipfs_drive_info){ .mounted = true };
}

static uint64_t backend_completion_count(enum phipfs_volume volume)
{
    assert(volume == PHIPFS_VOLUME_DATA && cpu_interrupts_enabled());
    (void)phipfs_has_atomic_replace(volume);
    return 77U;
}

static enum phipfs_status backend_replace(enum phipfs_volume volume, const char *from, const char *to)
{
    (void)volume; (void)from; (void)to;
    assert(false);
    return PHIPFS_STATUS_ACCESS;
}

static void observe_mount(void)
{
    do {
        assert(phipfs_drive(PHIPFS_VOLUME_DATA).mounted);
        assert(phipfs_completion_count(PHIPFS_VOLUME_DATA) == 77U);
        (void)phipfs_has_atomic_replace(PHIPFS_VOLUME_DATA);
        assert(cpu_interrupts_enabled());
        __atomic_fetch_add(&observations, 1U, __ATOMIC_RELEASE);
    } while (!__atomic_load_n(&observer_done, __ATOMIC_ACQUIRE));
}

static enum phipfs_status backend_sync(enum phipfs_volume volume)
{
    assert(cpu_interrupts_enabled() && phipfs_unmount(volume) == PHIPFS_STATUS_BUSY);
    return PHIPFS_STATUS_IO;
}

static enum phipfs_status backend_stat(enum phipfs_volume volume, const char *path, struct phipfs_stat *stat)
{
    assert(cpu_interrupts_enabled() && text_equal(path, "held"));
    assert(phipfs_unmount(volume) == PHIPFS_STATUS_BUSY);
    if (lookup_fails) return PHIPFS_STATUS_IO;
    *stat = (struct phipfs_stat){ .object_id = 500U, .size = 1700U };
    return PHIPFS_STATUS_OK;
}

static void exercise_mount(size_t worker)
{
    results[worker] = phase < 2U ? phipfs_mount(PHIPFS_VOLUME_DATA) : phipfs_unmount(PHIPFS_VOLUME_DATA);
    assert(cpu_interrupts_enabled());
    __atomic_fetch_add(&completed, 1U, __ATOMIC_RELEASE);
}

#ifdef _WIN32
static DWORD WINAPI observer_main(LPVOID argument)
{
    (void)argument;
    observe_mount();
    return 0U;
}

static DWORD WINAPI worker_main(LPVOID argument)
{
    exercise_mount((size_t)(uintptr_t)argument);
    return 0U;
}
#else
static void *observer_main(void *argument)
{
    (void)argument;
    observe_mount();
    return NULL;
}

static void *worker_main(void *argument)
{
    exercise_mount((size_t)(uintptr_t)argument);
    return NULL;
}
#endif

int main(void)
{
    static const struct vfs_backend_ops backend = {
        .mount = backend_transition, .unmount = backend_transition, .drive = backend_drive, .sync = backend_sync,
        .completion_count = backend_completion_count, .rename_replace = backend_replace,
        .stat_path = backend_stat, .validates_mutation_paths = true, .case_sensitive = true };
    volume_backends[PHIPFS_VOLUME_DATA] = &backend;
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) vnode_buckets[index] = VFS_NO_INDEX;
    const uint64_t generation_before = next_mount_generation;
    for (phase = 0U; phase < 4U; ++phase) {
        completed = backend_calls = 0U;
        backend_entered = release_backend = false;
        observer_done = false;
        observations = 0U;
#ifdef _WIN32
        HANDLE workers[WORKERS];
        HANDLE observer = CreateThread(NULL, 0U, observer_main, NULL, 0U, NULL);
        assert(observer != NULL);
#else
        pthread_t workers[WORKERS];
        pthread_t observer;
        assert(pthread_create(&observer, NULL, observer_main, NULL) == 0);
#endif
        for (size_t index = 0U; index < WORKERS; ++index) {
#ifdef _WIN32
            workers[index] = CreateThread(NULL, 0U, worker_main, (void *)(uintptr_t)index, 0U, NULL);
            assert(workers[index] != NULL);
#else
            assert(pthread_create(&workers[index], NULL, worker_main, (void *)(uintptr_t)index) == 0);
#endif
        }
        while (!__atomic_load_n(&backend_entered, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&completed, __ATOMIC_ACQUIRE) != WORKERS - 1U) yield_worker();
        assert(__atomic_load_n(&backend_calls, __ATOMIC_RELAXED) == 1U);
        assert(!phipfs_has_atomic_replace(PHIPFS_VOLUME_DATA));
        while (__atomic_load_n(&observations, __ATOMIC_ACQUIRE) < 100U) yield_worker();
        __atomic_store_n(&release_backend, true, __ATOMIC_RELEASE);
        for (size_t index = 0U; index < WORKERS; ++index) {
#ifdef _WIN32
            assert(WaitForSingleObject(workers[index], INFINITE) == WAIT_OBJECT_0);
            assert(CloseHandle(workers[index]));
#else
            assert(pthread_join(workers[index], NULL) == 0);
#endif
        }
        __atomic_store_n(&observer_done, true, __ATOMIC_RELEASE);
#ifdef _WIN32
        assert(WaitForSingleObject(observer, INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(observer));
#else
        assert(pthread_join(observer, NULL) == 0);
#endif
        unsigned owned = 0U;
        for (size_t index = 0U; index < WORKERS; ++index) {
            if (results[index] == PHIPFS_STATUS_BUSY) continue;
            assert(results[index] == (phase % 2U == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK));
            ++owned;
        }
        assert(owned == 1U && !mounts[PHIPFS_VOLUME_DATA].mounting && !mounts[PHIPFS_VOLUME_DATA].unmounting);
        assert(mounts[PHIPFS_VOLUME_DATA].active == (phase == 1U || phase == 2U));
        assert(phipfs_has_atomic_replace(PHIPFS_VOLUME_DATA) == (phase == 1U || phase == 2U));
        assert(next_mount_generation == generation_before + (phase == 0U ? 0U : 1U));
        if (phase == 1U || phase == 2U) {
            assert(mounts[PHIPFS_VOLUME_DATA].generation == generation_before);
            const struct vfs_backend_ops *pinned;
            assert(mount_pin(PHIPFS_VOLUME_DATA, &pinned) == PHIPFS_STATUS_OK && pinned == &backend);
            assert(phipfs_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_BUSY);
            mount_release(PHIPFS_VOLUME_DATA);
            assert(phipfs_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
            assert(mounts[PHIPFS_VOLUME_DATA].references == 0U);
            struct phipfs_stat stat;
            lookup_fails = false;
            assert(phipfs_stat_path(PHIPFS_VOLUME_DATA, "held", &stat) == PHIPFS_STATUS_OK);
            assert(stat.object_id == 500U && stat.size == 1700U);
            lookup_fails = true;
            assert(phipfs_stat_path(PHIPFS_VOLUME_DATA, "held", &stat) == PHIPFS_STATUS_IO);
            assert(stat.object_id == 0U && stat.size == 0U);
            assert(mounts[PHIPFS_VOLUME_DATA].references == 0U && vnode_resources_released());
        }
    }
    assert(phipfs_resources_released());
    assert(!phipfs_drive(PHIPFS_VOLUME_COUNT).mounted);
    assert(phipfs_completion_count(PHIPFS_VOLUME_COUNT) == 0U);
    assert(!phipfs_has_atomic_replace(PHIPFS_VOLUME_COUNT));
    puts("VFS concurrent mount/unmount: one owner, rollback, generations, concurrent observers and reentrant callbacks PASS");
    return 0;
}
