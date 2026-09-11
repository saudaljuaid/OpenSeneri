/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <phipia/native_handle.h>

static unsigned calls;
static enum native_resource_close_result next_result;
static enum native_resource_close_result close_file(uint8_t type,
    const struct native_resource *resource, void *context)
{
    assert(type == PHIPIA_HANDLE_FILE && resource->words[0] == 77U && context == &calls);
    ++calls;
    return next_result;
}

int main(void)
{
    struct native_handle_table table;
    const struct native_resource resource = {{77U, 0U, 0U, 0U}};
    struct native_resource *resolved;
    phipia_handle_t first, duplicate, reused;
    assert(native_handle_table_initialize(&table, 2U) == NATIVE_HANDLE_OK);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE, &resource, &first) == NATIVE_HANDLE_OK);
    assert(native_handle_duplicate(&table, first, &duplicate) == NATIVE_HANDLE_OK);
    next_result = NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    assert(native_handle_close(&table, first, close_file, &calls) == NATIVE_HANDLE_OK);
    assert(calls == 0U && table.active_handles == 1U && table.active_objects == 1U);
    // The VFS identity is already consumed. Return its error and retire the
    // native wrapper; neither close nor close_all may call the backend again.
    assert(native_handle_close(&table, duplicate, close_file, &calls) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(calls == 1U && table.active_handles == 0U && table.active_objects == 0U);
    assert(native_handle_resolve(&table, duplicate, PHIPIA_HANDLE_FILE, &resolved) == NATIVE_HANDLE_STALE);
    assert(native_handle_close_all(&table, close_file, &calls) == NATIVE_HANDLE_OK && calls == 1U);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE, &resource, &reused) == NATIVE_HANDLE_OK);
    assert(reused != first && reused != duplicate);
    // A refusal before consumption retains exactly one retryable reference.
    next_result = NATIVE_RESOURCE_RETAINED;
    assert(native_handle_close(&table, reused, close_file, &calls) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(table.active_handles == 1U && table.active_objects == 1U);
    assert(native_handle_resolve(&table, reused, PHIPIA_HANDLE_FILE, &resolved) == NATIVE_HANDLE_OK);
    next_result = NATIVE_RESOURCE_CLOSED;
    assert(native_handle_close(&table, reused, close_file, &calls) == NATIVE_HANDLE_OK);
    assert(calls == 3U && table.active_handles == 0U && table.active_objects == 0U);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE, &resource, &first) == NATIVE_HANDLE_OK);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE, &resource, &duplicate) == NATIVE_HANDLE_OK);
    next_result = NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    assert(native_handle_close_all(&table, close_file, &calls) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(calls == 5U && table.active_handles == 0U && table.active_objects == 0U);
    assert(native_handle_close_all(&table, close_file, &calls) == NATIVE_HANDLE_OK && calls == 5U);
    puts("native file close: consumed I/O errors retire wrappers, pre-consumption refusals retry, duplicate references and shutdown census PASS");
    return 0;
}
