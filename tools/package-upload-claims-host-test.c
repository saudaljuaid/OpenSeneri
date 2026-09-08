/* SPDX-License-Identifier: GPL-3.0-only */
/* Production upload requests with the existing filesystem/SHA test model. */
#include <assert.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

static void upload_claim_callback(void);
int upload_existing_host_main(void);
#define PACKAGE_UPLOAD_CLAIM_TEST
#define main upload_existing_host_main
#include "package-upload-host-test.c"
#undef main

#define UPLOAD_WORKERS 16U
#define UPLOAD_ROUNDS 500U
static package_upload_token shared_upload;
static unsigned workers_ready, successful_closes, stale_closes;
static bool workers_start, close_phase, check_reentrancy;

static void upload_yield(void)
{
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static void upload_claim_callback(void)
{
    if (!check_reentrancy) return;
    struct package_upload_report report;
    uint8_t byte = 0U, digest[PACKAGE_STATE_SHA256_BYTES] = {0};
    size_t count = 99U;
    assert(package_upload_initialize(&report) == PACKAGE_UPLOAD_STATUS_BUSY);
    assert(package_upload_open(101U, &report) == PACKAGE_UPLOAD_STATUS_BUSY);
    assert(package_upload_write(100U, shared_upload, &byte, 1U, &count,
        &report) == PACKAGE_UPLOAD_STATUS_BUSY && count == 0U);
    count = 99U;
    assert(package_upload_read(100U, shared_upload, 0U, &byte, 1U, &count,
        &report) == PACKAGE_UPLOAD_STATUS_BUSY && count == 0U);
    assert(package_upload_seal(100U, shared_upload, 1U, digest, &report) ==
        PACKAGE_UPLOAD_STATUS_BUSY);
    assert(package_upload_inspect(100U, shared_upload, &report) == PACKAGE_UPLOAD_STATUS_BUSY);
    assert(package_upload_close(100U, shared_upload, &report) == PACKAGE_UPLOAD_STATUS_BUSY);
    assert(report.token == 0U && !report.sealed && !report.durable);
    assert(!package_upload_resources_released());
}

static void upload_exercise(size_t worker)
{
    __atomic_fetch_add(&workers_ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&workers_start, __ATOMIC_ACQUIRE)) upload_yield();
    struct package_upload_report report;
    enum package_upload_status status;
    if (close_phase) {
        do {
            status = package_upload_close(100U, shared_upload, &report);
            if (status == PACKAGE_UPLOAD_STATUS_BUSY) upload_yield();
        } while (status == PACKAGE_UPLOAD_STATUS_BUSY);
        assert(status == PACKAGE_UPLOAD_STATUS_OK || status == PACKAGE_UPLOAD_STATUS_STALE);
        __atomic_fetch_add(status == PACKAGE_UPLOAD_STATUS_OK ?
            &successful_closes : &stale_closes, 1U, __ATOMIC_RELAXED);
        return;
    }
    for (size_t round = 0U; round < UPLOAD_ROUNDS; ++round) {
        const uint64_t record = (uint64_t)worker << 32U | round;
        size_t count;
        do {
            count = 99U;
            status = package_upload_write(100U, shared_upload,
                (const uint8_t *)&record, sizeof(record), &count, &report);
            if (status == PACKAGE_UPLOAD_STATUS_BUSY) {
                assert(count == 0U && report.token == 0U && !report.sealed);
                upload_yield();
            }
        } while (status == PACKAGE_UPLOAD_STATUS_BUSY);
        assert(status == PACKAGE_UPLOAD_STATUS_OK && count == sizeof(record));
        assert(report.token == shared_upload && !report.sealed && !report.durable);
        assert(report.byte_count >= (round + 1U) * sizeof(record) &&
            report.byte_count <= UPLOAD_WORKERS * UPLOAD_ROUNDS * sizeof(record));
    }
}

#ifdef _WIN32
static DWORD WINAPI upload_worker(LPVOID argument)
{
    upload_exercise((size_t)(uintptr_t)argument);
    return 0;
}
#else
static void *upload_worker(void *argument)
{
    upload_exercise((size_t)(uintptr_t)argument);
    return NULL;
}
#endif

static void compete(void)
{
    workers_ready = 0U;
    workers_start = false;
#ifdef _WIN32
    HANDLE threads[UPLOAD_WORKERS];
    for (size_t index = 0U; index < UPLOAD_WORKERS; ++index) {
        threads[index] = CreateThread(NULL, 0U, upload_worker, (LPVOID)(uintptr_t)index, 0U, NULL);
        assert(threads[index] != NULL);
    }
#else
    pthread_t threads[UPLOAD_WORKERS];
    for (size_t index = 0U; index < UPLOAD_WORKERS; ++index)
        assert(pthread_create(&threads[index], NULL, upload_worker, (void *)(uintptr_t)index) == 0);
#endif
    while (__atomic_load_n(&workers_ready, __ATOMIC_ACQUIRE) != UPLOAD_WORKERS) upload_yield();
    __atomic_store_n(&workers_start, true, __ATOMIC_RELEASE);
    for (size_t index = 0U; index < UPLOAD_WORKERS; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[index]));
#else
        assert(pthread_join(threads[index], NULL) == 0);
#endif
    }
}

int main(void)
{
    assert(upload_existing_host_main() == 0);
    struct package_upload_report report;
    size_t count;
    uint8_t byte = 1U;
    assert(package_upload_open(100U, &report) == PACKAGE_UPLOAD_STATUS_OK);
    shared_upload = report.token;
    check_reentrancy = true;
    assert(package_upload_write(100U, shared_upload, &byte, 1U, &count, &report) ==
        PACKAGE_UPLOAD_STATUS_OK && count == 1U);
    check_reentrancy = false;
    assert(package_upload_close(100U, shared_upload, &report) == PACKAGE_UPLOAD_STATUS_OK);

    assert(package_upload_open(100U, &report) == PACKAGE_UPLOAD_STATUS_OK);
    shared_upload = report.token;
    compete();
    assert(files[0].size == UPLOAD_WORKERS * UPLOAD_ROUNDS * sizeof(uint64_t));
    bool seen[UPLOAD_WORKERS][UPLOAD_ROUNDS] = {{false}};
    for (size_t offset = 0U; offset < files[0].size; offset += sizeof(uint64_t)) {
        uint64_t record;
        memcpy(&record, files[0].bytes + offset, sizeof(record));
        const size_t worker = (size_t)(record >> 32U), round = (uint32_t)record;
        assert(worker < UPLOAD_WORKERS && round < UPLOAD_ROUNDS && !seen[worker][round]);
        seen[worker][round] = true;
    }
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    assert(package_state_sha256(files[0].bytes, files[0].size, digest) == PACKAGE_STATE_STATUS_OK);
    assert(package_upload_seal(100U, shared_upload, files[0].size, digest, &report) ==
        PACKAGE_UPLOAD_STATUS_OK && report.sealed && report.durable);
    close_phase = true;
    compete();
    assert(successful_closes == 1U && stale_closes == UPLOAD_WORKERS - 1U);
    assert(package_upload_resources_released());
    for (size_t index = 0U; index < PACKAGE_UPLOAD_SLOT_LIMIT; ++index)
        assert(!files[index].present && !files[index].open);
    puts("package upload claims: reentrant BUSY, 16 writers, 8000 exact records and digest, one close owner and empty census PASS");
    return 0;
}
