/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>

#include <phipia/native_handle.h>
#include <phipia/native_process.h>

struct report_script {
    enum native_resource_close_result results[PHIPIA_HANDLE_PACKAGE_CONTROL + 1U];
    unsigned calls[PHIPIA_HANDLE_PACKAGE_CONTROL + 1U];
};

static void script_initialize(
    struct report_script *script,
    enum native_resource_close_result result
)
{
    assert(script != NULL);
    for (size_t index = 0U; index < sizeof(*script); ++index) {
        ((uint8_t *)script)[index] = 0U;
    }
    for (size_t type = PHIPIA_HANDLE_FILE;
         type <= PHIPIA_HANDLE_PACKAGE_CONTROL; ++type) {
        script->results[type] = result;
    }
}

static enum native_resource_close_result scripted_close(
    uint8_t type,
    const struct native_resource *resource,
    void *context
)
{
    struct report_script *script = context;

    assert(script != NULL && resource != NULL);
    assert(type >= PHIPIA_HANDLE_FILE &&
        type <= PHIPIA_HANDLE_PACKAGE_CONTROL);
    ++script->calls[type];
    return script->results[type];
}

static void install_resource(
    struct native_handle_table *table,
    uint8_t type,
    phipia_handle_t *handle
)
{
    const struct native_resource resource = {{
        UINT64_C(0x70000000) + type, UINT64_C(0xABC00000) + type,
        UINT64_C(0x12340000) + type, UINT64_C(0xF0000000) + type
    }};

    assert(native_handle_install(table, type, &resource, handle) ==
        NATIVE_HANDLE_OK);
}

static const struct native_handle_close_report_entry *report_entry_at(
    const struct native_handle_close_report *report,
    uint16_t index
)
{
    assert(report != NULL && index < report->attempted_handles &&
        index < NATIVE_HANDLE_CLOSE_REPORT_CAPACITY);
    return &report->entries[index];
}

static bool report_has_outcome(
    const struct native_handle_close_report *report,
    uint8_t type,
    enum native_handle_close_outcome outcome
)
{
    const uint16_t count = report->attempted_handles <
        NATIVE_HANDLE_CLOSE_REPORT_CAPACITY ? report->attempted_handles :
        NATIVE_HANDLE_CLOSE_REPORT_CAPACITY;

    for (uint16_t index = 0U; index < count; ++index) {
        if (report->entries[index].type == type &&
            report->entries[index].outcome == (uint8_t)outcome) {
            return true;
        }
    }
    return false;
}

static void assert_report_empty(
    const struct native_handle_close_report *report
)
{
    assert(report != NULL);
    assert(report->attempted_handles == 0U &&
        report->callback_attempts == 0U && report->closed_resources == 0U &&
        report->consumed_error_resources == 0U &&
        report->retained_resources == 0U && report->stale_entries == 0U &&
        report->invalid_entries == 0U && report->invalid_arguments == 0U &&
        report->duplicate_references == 0U && report->retired_handles == 0U &&
        report->omitted_entries == 0U && !report->retryable &&
        !report->progress && !report->truncated);
}

static void report_argument_and_reset_test(void)
{
    struct native_handle_close_report report;
    struct native_handle_table table = {0};
    phipia_handle_t handle = PHIPIA_HANDLE_INVALID;

    for (size_t index = 0U; index < sizeof(report); ++index) {
        ((uint8_t *)&report)[index] = UINT8_MAX;
    }
    assert(native_handle_close_all_diagnostics(NULL, scripted_close, NULL,
        &report) == NATIVE_HANDLE_NULL_ARGUMENT);
    assert(report.status == NATIVE_HANDLE_NULL_ARGUMENT &&
        report.invalid_arguments == 1U && report.attempted_handles == 0U);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, NULL,
        &report) == NATIVE_HANDLE_BAD_LIMIT);
    assert(report.status == NATIVE_HANDLE_BAD_LIMIT &&
        report.invalid_arguments == 1U && report.attempted_handles == 0U);

    assert(native_handle_table_initialize(&table, 1U) == NATIVE_HANDLE_OK);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE,
        &(const struct native_resource){{1U, 2U, 3U, 4U}}, &handle) ==
        NATIVE_HANDLE_OK);
    assert(native_handle_close_all_diagnostics(&table, NULL, NULL, &report) ==
        NATIVE_HANDLE_OK);
    assert(report.status == NATIVE_HANDLE_OK && report.closed_resources == 1U &&
        report.retired_handles == 1U && report.progress && !report.truncated);
    native_handle_close_report_reset(&report);
    assert_report_empty(&report);
    assert(report.status == NATIVE_HANDLE_OK);
    assert(native_handle_close_all_diagnostics(&table, NULL, NULL, NULL) ==
        NATIVE_HANDLE_NULL_ARGUMENT);
}

static void mixed_resource_report_test(void)
{
    static const uint8_t types[] = {
        PHIPIA_HANDLE_FILE, PHIPIA_HANDLE_DIRECTORY, PHIPIA_HANDLE_WINDOW,
        PHIPIA_HANDLE_EVENT_QUEUE, PHIPIA_HANDLE_STREAM,
        PHIPIA_HANDLE_DATAGRAM, PHIPIA_HANDLE_TIMER, PHIPIA_HANDLE_THREAD,
        PHIPIA_HANDLE_AUDIO_OUTPUT, PHIPIA_HANDLE_PACKAGE_UPLOAD,
        PHIPIA_HANDLE_PACKAGE_CONTROL
    };
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct report_script script;
    phipia_handle_t handles[sizeof(types) / sizeof(types[0])];
    phipia_handle_t duplicate;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    script.results[PHIPIA_HANDLE_DIRECTORY] =
        NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    script.results[PHIPIA_HANDLE_WINDOW] = NATIVE_RESOURCE_RETAINED;
    script.results[PHIPIA_HANDLE_PACKAGE_UPLOAD] =
        NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    assert(native_handle_table_initialize(&table, 12U) == NATIVE_HANDLE_OK);
    for (size_t index = 0U; index < sizeof(types) / sizeof(types[0]); ++index) {
        install_resource(&table, types[index], &handles[index]);
    }
    assert(native_handle_duplicate(&table, handles[0], &duplicate) ==
        NATIVE_HANDLE_OK);

    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(report.status == NATIVE_HANDLE_CLOSE_FAILED &&
        report.attempted_handles == 12U && report.callback_attempts == 11U &&
        report.closed_resources == 8U &&
        report.consumed_error_resources == 2U &&
        report.retained_resources == 1U && report.duplicate_references == 1U &&
        report.stale_entries == 0U && report.invalid_entries == 0U &&
        report.retired_handles == 11U && report.active_handles_before == 12U &&
        report.active_handles_after == 1U && report.retryable &&
        report.progress && !report.truncated);
    assert(script.calls[PHIPIA_HANDLE_FILE] == 1U &&
        script.calls[PHIPIA_HANDLE_DIRECTORY] == 1U &&
        script.calls[PHIPIA_HANDLE_WINDOW] == 1U &&
        script.calls[PHIPIA_HANDLE_AUDIO_OUTPUT] == 1U &&
        script.calls[PHIPIA_HANDLE_PACKAGE_UPLOAD] == 1U &&
        report_has_outcome(&report, PHIPIA_HANDLE_FILE,
            NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE) &&
        report_has_outcome(&report, PHIPIA_HANDLE_DIRECTORY,
            NATIVE_HANDLE_CLOSE_OUTCOME_CONSUMED_ERROR) &&
        report_has_outcome(&report, PHIPIA_HANDLE_WINDOW,
            NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED));
    assert(table.active_handles == 1U && table.active_objects == 1U);
    assert(script.calls[PHIPIA_HANDLE_DIRECTORY] == 1U &&
        script.calls[PHIPIA_HANDLE_PACKAGE_UPLOAD] == 1U);

    script.results[PHIPIA_HANDLE_WINDOW] = NATIVE_RESOURCE_CLOSED;
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert(report.status == NATIVE_HANDLE_OK && report.attempted_handles == 1U &&
        report.callback_attempts == 1U && report.closed_resources == 1U &&
        report.consumed_error_resources == 0U &&
        report.retained_resources == 0U && report.duplicate_references == 0U &&
        report.active_handles_before == 1U && report.active_handles_after == 0U &&
        report.progress && !report.retryable &&
        report_entry_at(&report, 0U)->type == PHIPIA_HANDLE_WINDOW &&
        report_entry_at(&report, 0U)->outcome ==
            NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED);
    assert(script.calls[PHIPIA_HANDLE_WINDOW] == 2U &&
        script.calls[PHIPIA_HANDLE_DIRECTORY] == 1U &&
        script.calls[PHIPIA_HANDLE_PACKAGE_UPLOAD] == 1U);
}

static void duplicate_retry_progress_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report first_report;
    struct native_handle_close_report second_report;
    struct report_script script;
    phipia_handle_t first;
    phipia_handle_t second;
    phipia_handle_t third;
    phipia_handle_t fourth;

    script_initialize(&script, NATIVE_RESOURCE_RETAINED);
    assert(native_handle_table_initialize(&table, 4U) == NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_FILE, &first);
    assert(native_handle_duplicate(&table, first, &second) == NATIVE_HANDLE_OK);
    assert(native_handle_duplicate(&table, first, &third) == NATIVE_HANDLE_OK);
    assert(native_handle_duplicate(&table, first, &fourth) == NATIVE_HANDLE_OK);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &first_report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(first_report.attempted_handles == 4U &&
        first_report.callback_attempts == 1U &&
        first_report.duplicate_references == 3U &&
        first_report.retained_resources == 1U &&
        first_report.retired_handles == 3U && first_report.progress &&
        first_report.retryable && first_report.active_handles_after == 1U &&
        script.calls[PHIPIA_HANDLE_FILE] == 1U);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &second_report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(second_report.attempted_handles == 1U &&
        second_report.callback_attempts == 1U &&
        second_report.duplicate_references == 0U &&
        second_report.retained_resources == 1U && !second_report.progress &&
        second_report.retryable && second_report.active_handles_after == 1U &&
        script.calls[PHIPIA_HANDLE_FILE] == 2U);
    script.results[PHIPIA_HANDLE_FILE] = NATIVE_RESOURCE_CLOSED;
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &second_report) == NATIVE_HANDLE_OK);
    assert(second_report.attempted_handles == 1U &&
        second_report.closed_resources == 1U && second_report.progress &&
        !second_report.retryable && second_report.active_handles_after == 0U &&
        script.calls[PHIPIA_HANDLE_FILE] == 3U);
}

static void ordinary_close_report_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct report_script script;
    phipia_handle_t first;
    phipia_handle_t duplicate;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED_WITH_ERROR);
    assert(native_handle_table_initialize(&table, 2U) == NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_FILE, &first);
    assert(native_handle_duplicate(&table, first, &duplicate) ==
        NATIVE_HANDLE_OK);
    assert(native_handle_close_with_report(&table, first, scripted_close,
        &script, &report) == NATIVE_HANDLE_OK);
    assert(report.status == NATIVE_HANDLE_OK && report.attempted_handles == 1U &&
        report.callback_attempts == 0U && report.duplicate_references == 1U &&
        report.retired_handles == 1U && report.progress &&
        report.active_handles_after == 1U &&
        report_entry_at(&report, 0U)->outcome ==
            NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE &&
        script.calls[PHIPIA_HANDLE_FILE] == 0U);
    assert(native_handle_close_with_report(&table, duplicate, scripted_close,
        &script, &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(report.status == NATIVE_HANDLE_CLOSE_FAILED &&
        report.callback_attempts == 1U && report.consumed_error_resources == 1U &&
        report.retired_handles == 1U && report.active_handles_after == 0U &&
        report.progress && !report.retryable &&
        script.calls[PHIPIA_HANDLE_FILE] == 1U);
    assert(native_handle_close_with_report(&table, duplicate, scripted_close,
        &script, &report) == NATIVE_HANDLE_STALE);
    assert(report.status == NATIVE_HANDLE_STALE && report.attempted_handles == 1U &&
        report.stale_entries == 1U && report.callback_attempts == 0U &&
        report.active_handles_before == 0U && report.active_handles_after == 0U);
    assert(native_handle_close_with_report(&table, PHIPIA_HANDLE_INVALID,
        scripted_close, &script, &report) == NATIVE_HANDLE_STALE);
    assert(report.status == NATIVE_HANDLE_STALE && report.invalid_entries == 1U &&
        report.attempted_handles == 1U && report.callback_attempts == 0U &&
        !report.progress);
    assert(native_handle_close_with_report(&table,
        first | UINT64_C(0x01000000), scripted_close, &script, &report) ==
        NATIVE_HANDLE_STALE);
    assert(report.status == NATIVE_HANDLE_STALE &&
        report.invalid_entries == 1U && report.stale_entries == 0U &&
        report.callback_attempts == 0U);
    assert(native_handle_close_with_report(&table,
        (UINT64_C(1) << 32) | ((uint64_t)PHIPIA_HANDLE_FILE << 16) | 3U,
        scripted_close, &script, &report) == NATIVE_HANDLE_STALE);
    assert(report.invalid_entries == 1U && report.stale_entries == 0U &&
        report.callback_attempts == 0U);
}

static void malformed_entry_report_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct report_script script;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    assert(native_handle_table_initialize(&table, 3U) == NATIVE_HANDLE_OK);
    table.active_handles = 3U;
    table.slots[0].active = true;
    table.slots[0].generation = 1U;
    table.slots[0].type = PHIPIA_HANDLE_FILE;
    table.slots[0].object_index = 3U;
    table.slots[1].active = true;
    table.slots[1].generation = 1U;
    table.slots[1].type = PHIPIA_HANDLE_DIRECTORY;
    table.slots[1].object_index = 0U;
    table.slots[2].active = true;
    table.slots[2].generation = 0U;
    table.slots[2].type = UINT8_C(0xFE);
    table.slots[2].object_index = 0U;

    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(report.status == NATIVE_HANDLE_CLOSE_FAILED &&
        report.attempted_handles == 3U && report.callback_attempts == 0U &&
        report.stale_entries == 1U && report.invalid_entries == 2U &&
        report.retained_resources == 0U && report.retired_handles == 0U &&
        report.active_handles_before == 3U && report.active_handles_after == 3U &&
        !report.retryable && !report.progress &&
        report_has_outcome(&report, PHIPIA_HANDLE_DIRECTORY,
            NATIVE_HANDLE_CLOSE_OUTCOME_STALE) &&
        report_has_outcome(&report, UINT8_C(0xFE),
            NATIVE_HANDLE_CLOSE_OUTCOME_INVALID));
    assert(script.calls[PHIPIA_HANDLE_FILE] == 0U &&
        script.calls[PHIPIA_HANDLE_DIRECTORY] == 0U);
    assert(native_handle_close_all(&table, scripted_close, &script) ==
        NATIVE_HANDLE_CLOSE_FAILED);
}

static void bounded_capacity_report_test(void)
{
    static struct native_handle_table table;
    struct native_handle_close_report report;
    struct report_script script;
    phipia_handle_t handle;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    assert(native_handle_table_initialize(&table, NATIVE_HANDLE_LIMIT) ==
        NATIVE_HANDLE_OK);
    for (size_t index = 0U; index < NATIVE_HANDLE_LIMIT; ++index) {
        install_resource(&table, PHIPIA_HANDLE_TIMER, &handle);
    }
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert(report.attempted_handles == NATIVE_HANDLE_LIMIT &&
        report.callback_attempts == NATIVE_HANDLE_LIMIT &&
        report.closed_resources == NATIVE_HANDLE_LIMIT &&
        report.retired_handles == NATIVE_HANDLE_LIMIT &&
        report.omitted_entries == NATIVE_HANDLE_LIMIT -
            NATIVE_HANDLE_CLOSE_REPORT_CAPACITY && report.truncated &&
        report.active_handles_before == NATIVE_HANDLE_LIMIT &&
        report.active_handles_after == 0U && report.progress &&
        !report.retryable && script.calls[PHIPIA_HANDLE_TIMER] ==
            NATIVE_HANDLE_LIMIT);
    for (size_t index = 0U; index < NATIVE_HANDLE_CLOSE_REPORT_CAPACITY;
         ++index) {
        assert(report.entries[index].type == PHIPIA_HANDLE_TIMER &&
            report.entries[index].outcome ==
                NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED);
    }
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert_report_empty(&report);
    assert(report.active_handles_before == 0U &&
        report.active_handles_after == 0U && report.status == NATIVE_HANDLE_OK);
}

static void process_report_surface_test(void)
{
    struct native_process_result result = {0};

    result.teardown_report.attempts = 1U;
    result.teardown_report.blocked = true;
    result.teardown_report.retired = false;
    result.teardown_report.handles.status = NATIVE_HANDLE_CLOSE_FAILED;
    result.teardown_report.handles.retained_resources = 1U;
    result.teardown_report.handles.retryable = true;
    result.teardown_report.handles.active_handles_after = 1U;
    assert(result.teardown_report.attempts == 1U &&
        result.teardown_report.blocked && !result.teardown_report.retired &&
        result.teardown_report.handles.retryable &&
        result.teardown_report.handles.active_handles_after == 1U);

    result.teardown_report.attempts = 2U;
    result.teardown_report.blocked = false;
    result.teardown_report.retired = true;
    result.teardown_report.handles.status = NATIVE_HANDLE_CLOSE_FAILED;
    result.teardown_report.handles.retained_resources = 0U;
    result.teardown_report.handles.consumed_error_resources = 1U;
    result.teardown_report.handles.retryable = false;
    result.teardown_report.handles.progress = true;
    result.teardown_report.handles.active_handles_after = 0U;
    assert(result.teardown_report.attempts == 2U &&
        !result.teardown_report.blocked && result.teardown_report.retired &&
        result.teardown_report.handles.consumed_error_resources == 1U &&
        !result.teardown_report.handles.retryable &&
        result.teardown_report.handles.active_handles_after == 0U);
}

int main(void)
{
    report_argument_and_reset_test();
    mixed_resource_report_test();
    duplicate_retry_progress_test();
    ordinary_close_report_test();
    malformed_entry_report_test();
    bounded_capacity_report_test();
    process_report_surface_test();
    puts("native teardown report: bounded census, tri-state recovery, stale entries, and process report surface PASS");
    return 0;
}
