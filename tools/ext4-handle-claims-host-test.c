/* SPDX-License-Identifier: GPL-3.0-only */
/* Competing owners exercise the production shared-slot lifecycle directly.
 * This tests allocation ownership, not concurrent backend inode operations. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#include "../src/kernel/ext4_fs.c"

#define WORKERS (EXT4_MAX_HANDLES + 4U)
#define ROUNDS 500U
static unsigned ready;
static bool proceed;
static size_t initial_slots[WORKERS];
static uint64_t issued[WORKERS * ROUNDS];

static void yield_worker(void)
{
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static void exercise_claims(size_t worker)
{
    initial_slots[worker] = reserve_handle_slot();
    __atomic_fetch_add(&ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&proceed, __ATOMIC_ACQUIRE)) yield_worker();
    if (initial_slots[worker] != EXT4_MAX_HANDLES) retire_handle_slot(initial_slots[worker]);
    for (size_t round = 0U; round < ROUNDS; ++round) {
        size_t slot;
        do {
            slot = reserve_handle_slot();
            if (slot == EXT4_MAX_HANDLES) yield_worker();
        } while (slot == EXT4_MAX_HANDLES);
        assert(!ext4_handles[slot].active && ext4_handles[slot].inode == 0U);
        phipfs_handle handle = 0U;
        initialize_reserved_handle(slot, (enum phipfs_volume)(worker % PHIPFS_VOLUME_COUNT),
            "owned", worker + 1U, round, PHIPFS_ACCESS_READ, false, 0U, &handle);
        issued[worker * ROUNDS + round] = handle >> 8U;
        yield_worker();
        assert(ext4_handles[slot].active && ext4_handles[slot].inode == worker + 1U);
        assert(ext4_handles[slot].size == round && ext4_handles[slot].generation == (handle >> 8U));
        assert(__atomic_load_n(&ext4_handle_claims[slot], __ATOMIC_ACQUIRE));
        retire_handle_slot(slot);
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID argument)
{
    exercise_claims((size_t)(uintptr_t)argument);
    return 0U;
}
#else
static void *worker_main(void *argument)
{
    exercise_claims((size_t)(uintptr_t)argument);
    return NULL;
}
#endif

static int compare_generation(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

int main(void)
{
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
    bool occupied[EXT4_MAX_HANDLES] = { false };
    size_t admitted = 0U;
    for (size_t index = 0U; index < WORKERS; ++index) {
        const size_t slot = initial_slots[index];
        if (slot == EXT4_MAX_HANDLES) continue;
        assert(!occupied[slot]);
        occupied[slot] = true;
        ++admitted;
    }
    assert(admitted == EXT4_MAX_HANDLES && reserve_handle_slot() == EXT4_MAX_HANDLES);
    __atomic_store_n(&proceed, true, __ATOMIC_RELEASE);
    for (size_t index = 0U; index < WORKERS; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(workers[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(workers[index]));
#else
        assert(pthread_join(workers[index], NULL) == 0);
#endif
    }
    qsort(issued, WORKERS * ROUNDS, sizeof(uint64_t), compare_generation);
    for (size_t index = 0U; index < WORKERS * ROUNDS; ++index)
        assert(issued[index] != 0U && (index == 0U || issued[index - 1U] != issued[index]));
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index)
        assert(!ext4_handle_claims[index] && !ext4_handles[index].active);
    uint64_t wrap = UINT64_MAX >> 8U;
    assert(generation(&wrap) == (UINT64_MAX >> 8U));
    assert(generation(&wrap) == 1U && generation(&wrap) == 2U);
    puts("ext4 competing handle claims: exhaustion, unique ownership/generations and final reuse PASS");
    return 0;
}
