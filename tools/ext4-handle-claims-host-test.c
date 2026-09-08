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
#if defined(PHIPIA_TEST_VFS_FILES) || defined(PHIPIA_TEST_VFS_DIRECTORIES)
#include "../src/kernel/vfs.c"
#ifdef PHIPIA_TEST_VFS_FILES
#define TEST_CAPACITY VFS_MAX_OPEN_FILES
#define TEST_CLAIMS open_file_claims
#define TEST_STATES open_files
#define TEST_GENERATION next_open_generation
#define TEST_LABEL "VFS file"
#else
#define TEST_CAPACITY VFS_MAX_DIRECTORY_ITERATORS
#define TEST_CLAIMS directory_claims
#define TEST_STATES directories
#define TEST_GENERATION next_directory_generation
#define TEST_LABEL "VFS directory"
#endif
static size_t claim_test_slot(void) { return phipia_slot_claim(TEST_CLAIMS, TEST_CAPACITY); }
static void retire_test_slot(size_t slot)
{
    zero_bytes(&TEST_STATES[slot], sizeof(TEST_STATES[slot]));
    phipia_slot_release(TEST_CLAIMS, slot);
}
static uint64_t issue_generation(uint64_t *counter) { return next_generation(counter, UINT64_MAX >> 8U); }
static uint64_t initialize_test_slot(size_t slot, size_t worker, size_t round)
{
    assert(!TEST_STATES[slot].active && TEST_STATES[slot].backend_handle == 0U);
    TEST_STATES[slot].generation = issue_generation(&TEST_GENERATION);
    TEST_STATES[slot].backend_handle = worker + 1U;
    TEST_STATES[slot].vnode_generation = round;
    TEST_STATES[slot].active = true;
    return TEST_STATES[slot].generation;
}
static void check_test_slot(size_t slot, size_t worker, size_t round, uint64_t token)
{
    assert(TEST_STATES[slot].active && TEST_STATES[slot].backend_handle == worker + 1U);
    assert(TEST_STATES[slot].vnode_generation == round && TEST_STATES[slot].generation == token);
}
#else
#include "../src/kernel/ext4_fs.c"
#define TEST_CAPACITY EXT4_MAX_HANDLES
#define TEST_CLAIMS ext4_handle_claims
#define TEST_STATES ext4_handles
#define TEST_LABEL "ext4"
static size_t claim_test_slot(void) { return reserve_handle_slot(); }
static void retire_test_slot(size_t slot) { retire_handle_slot(slot); }
static uint64_t issue_generation(uint64_t *counter) { return generation(counter); }
static uint64_t initialize_test_slot(size_t slot, size_t worker, size_t round)
{
    assert(!ext4_handles[slot].active && ext4_handles[slot].inode == 0U);
    phipfs_handle handle = 0U;
    initialize_reserved_handle(slot, (enum phipfs_volume)(worker % PHIPFS_VOLUME_COUNT),
        "owned", worker + 1U, round, PHIPFS_ACCESS_READ, false, 0U, &handle);
    return handle >> 8U;
}
static void check_test_slot(size_t slot, size_t worker, size_t round, uint64_t token)
{
    assert(ext4_handles[slot].active && ext4_handles[slot].inode == worker + 1U);
    assert(ext4_handles[slot].size == round && ext4_handles[slot].generation == token);
}
#endif

#define WORKERS (TEST_CAPACITY + 4U)
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
    initial_slots[worker] = claim_test_slot();
    __atomic_fetch_add(&ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&proceed, __ATOMIC_ACQUIRE)) yield_worker();
    if (initial_slots[worker] != TEST_CAPACITY) retire_test_slot(initial_slots[worker]);
    for (size_t round = 0U; round < ROUNDS; ++round) {
        size_t slot;
        do {
            slot = claim_test_slot();
            if (slot == TEST_CAPACITY) yield_worker();
        } while (slot == TEST_CAPACITY);
        const uint64_t token = initialize_test_slot(slot, worker, round);
        issued[worker * ROUNDS + round] = token;
        yield_worker();
        check_test_slot(slot, worker, round, token);
        assert(__atomic_load_n(&TEST_CLAIMS[slot], __ATOMIC_ACQUIRE));
        retire_test_slot(slot);
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
    bool occupied[TEST_CAPACITY] = { false };
    size_t admitted = 0U;
    for (size_t index = 0U; index < WORKERS; ++index) {
        const size_t slot = initial_slots[index];
        if (slot == TEST_CAPACITY) continue;
        assert(!occupied[slot]);
        occupied[slot] = true;
        ++admitted;
    }
    assert(admitted == TEST_CAPACITY && claim_test_slot() == TEST_CAPACITY);
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
    for (size_t index = 0U; index < TEST_CAPACITY; ++index)
        assert(!TEST_CLAIMS[index] && !TEST_STATES[index].active);
    uint64_t wrap = UINT64_MAX >> 8U;
    assert(issue_generation(&wrap) == (UINT64_MAX >> 8U));
    assert(issue_generation(&wrap) == 1U && issue_generation(&wrap) == 2U);
    puts(TEST_LABEL " competing handle claims: exhaustion, unique ownership/generations and final reuse PASS");
    return 0;
}
