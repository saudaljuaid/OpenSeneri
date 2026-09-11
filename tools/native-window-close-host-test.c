/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <phipia/cpu.h>
#include <phipia/native_handle.h>
#include <phipia/native_process.h>
#include <phipia/ui.h>

static bool host_interrupts_enabled = true;
static bool host_window_open = true;
static unsigned host_window_close_calls;

bool cpu_interrupts_enabled(void)
{
    return host_interrupts_enabled;
}

void cpu_interrupt_disable(void)
{
    host_interrupts_enabled = false;
}

void cpu_interrupt_enable(void)
{
    host_interrupts_enabled = true;
}

bool ui_native_window_is_open(uint32_t slot)
{
    assert(slot == 3U);
    return host_window_open;
}

enum ui_status ui_native_window_close(uint32_t slot)
{
    assert(slot == 3U);
    ++host_window_close_calls;
    host_window_open = false;
    return UI_STATUS_SURFACE_FAILURE;
}

#include "../src/kernel/native_process.c"

int main(void)
{
    const uint64_t generation = UINT64_C(17);
    const struct native_resource resource = {{3U, generation, 0U, 0U}};
    struct native_process process;
    struct native_handle_table table;
    phipia_handle_t first;
    phipia_handle_t duplicate;

    zero_bytes(&process, sizeof(process));
    process.generation = generation;
    process.window.allocated = true;
    process.window.generation = generation;
    process.window.ui_slot = 3U;
    process.window.window_object_open = true;
    process.window.event_object_open = true;

    assert(native_handle_table_initialize(&table, 2U) == NATIVE_HANDLE_OK);
    assert(native_handle_install(&table, PHIPIA_HANDLE_WINDOW, &resource,
        &first) == NATIVE_HANDLE_OK);
    assert(native_handle_duplicate(&table, first, &duplicate) == NATIVE_HANDLE_OK);
    assert(native_handle_close(&table, first, close_resource, &process) ==
        NATIVE_HANDLE_OK);
    assert(host_window_close_calls == 0U && table.active_handles == 1U &&
        table.active_objects == 1U);
    assert(native_handle_close(&table, duplicate, close_resource, &process) ==
        NATIVE_HANDLE_CLOSE_FAILED);
    assert(host_window_close_calls == 1U && table.active_handles == 0U &&
        table.active_objects == 0U);
    assert(!process.window.window_object_open &&
        process.window.event_object_open);
    assert(native_handle_close_all(&table, close_resource, &process) ==
        NATIVE_HANDLE_OK && host_window_close_calls == 1U);
    puts("native window handle close: consumed UI teardown errors retire duplicate wrappers PASS");
    return 0;
}
