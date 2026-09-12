/* SPDX-License-Identifier: GPL-3.0-only */
#include <pthread.h>
#include <phipia/event.h>
#include <phipia/runtime.h>
#include <phipia/window.h>
#include <stdint.h>

static void *blocked_thread(void *argument)
{
    (void)argument;
    for (;;) {
        (void)phipia_sleep_until(phipia_monotonic_ns() +
            UINT64_C(1000000000));
    }
}

static _Noreturn void poison_state_and_fault(void)
{
    static const uint64_t pattern[2] = {
        UINT64_C(0xD15EA5E0D15EA5E0), UINT64_C(0x2EA15A1F2EA15A1F)
    };

    __asm__ volatile("movdqu %0, %%xmm15\n\tfldpi" : : "m" (pattern) :
        "xmm15", "memory");
    *(volatile uint64_t *)(uintptr_t)0U = UINT64_C(0xBADF00D);
    __builtin_unreachable();
}

int main(void)
{
    struct phipia_memory_map_response mapping = {0U, 0U, 0U, 0U};
    struct phipia_window_create_response window = {0U};
    pthread_t thread;
    long file;
    long directory;
    long timer;

    if (phipia_path_mkdir(PHIPIA_VOLUME_DATA, "LIVE") != 0) return 10;
    file = phipia_file_open(PHIPIA_VOLUME_DATA, "LIVE/OPEN.TXT",
        PHIPIA_OPEN_WRITE | PHIPIA_OPEN_CREATE | PHIPIA_OPEN_TRUNCATE);
    directory = phipia_directory_open(PHIPIA_VOLUME_DATA, "LIVE");
    timer = phipia_timer_create();
    if (file < 0 || directory < 0 || timer < 0 ||
        phipia_file_write((phipia_handle_t)file, "live", 4U) != 4 ||
        phipia_timer_set((phipia_handle_t)timer,
            phipia_monotonic_ns() + UINT64_C(1000000000)) != 0 ||
        phipia_memory_allocate(2U * PHIPIA_ABI_PAGE_SIZE,
            PHIPIA_MEMORY_READ | PHIPIA_MEMORY_WRITE, &mapping) != 0 ||
        phipia_window_create("Crash containment", 160U, 96U, &window) != 0 ||
        pthread_create(&thread, NULL, blocked_thread, NULL) != 0) {
        return 11;
    }
    poison_state_and_fault();
}
