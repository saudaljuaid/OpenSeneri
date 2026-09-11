/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <stdio.h>

#include <phipia/native_handle.h>
#include <phipia/native_process.h>

static const uint8_t all_types[] = {
    PHIPIA_HANDLE_FILE,
    PHIPIA_HANDLE_DIRECTORY,
    PHIPIA_HANDLE_WINDOW,
    PHIPIA_HANDLE_EVENT_QUEUE,
    PHIPIA_HANDLE_STREAM,
    PHIPIA_HANDLE_DATAGRAM,
    PHIPIA_HANDLE_TIMER,
    PHIPIA_HANDLE_THREAD,
    PHIPIA_HANDLE_AUDIO_OUTPUT,
    PHIPIA_HANDLE_PACKAGE_UPLOAD,
    PHIPIA_HANDLE_PACKAGE_CONTROL
};

struct close_script {
    enum native_resource_close_result results[NATIVE_HANDLE_CLOSE_TYPE_COUNT];
    unsigned calls[NATIVE_HANDLE_CLOSE_TYPE_COUNT];
    uint8_t order[NATIVE_HANDLE_LIMIT];
    size_t order_count;
};

static void clear_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void script_initialize(
    struct close_script *script,
    enum native_resource_close_result result
)
{
    assert(script != NULL);
    clear_bytes(script, sizeof(*script));
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
    struct close_script *script = context;

    assert(script != NULL && resource != NULL);
    assert(type >= PHIPIA_HANDLE_FILE &&
        type <= PHIPIA_HANDLE_PACKAGE_CONTROL);
    ++script->calls[type];
    if (script->order_count < NATIVE_HANDLE_LIMIT) {
        script->order[script->order_count++] = type;
    }
    return script->results[type];
}

static void install_resource(
    struct native_handle_table *table,
    uint8_t type,
    uint64_t marker,
    phipia_handle_t *handle
)
{
    const struct native_resource resource = {{
        marker, marker + UINT64_C(1), marker + UINT64_C(2),
        marker + UINT64_C(3)
    }};

    assert(native_handle_install(table, type, &resource, handle) ==
        NATIVE_HANDLE_OK);
}

static const struct native_handle_close_type_summary *summary_for(
    const struct native_handle_close_report *report,
    uint8_t type
)
{
    assert(report != NULL && type < NATIVE_HANDLE_CLOSE_TYPE_COUNT);
    return &report->types[type];
}

static void assert_summary_zero(
    const struct native_handle_close_type_summary *summary
)
{
    assert(summary != NULL);
    assert(summary->attempted_handles == 0U &&
        summary->callback_attempts == 0U &&
        summary->closed_resources == 0U &&
        summary->consumed_error_resources == 0U &&
        summary->retained_resources == 0U &&
        summary->duplicate_references == 0U &&
        summary->stale_entries == 0U &&
        summary->invalid_entries == 0U &&
        summary->retired_handles == 0U);
}

static void assert_report_counters_zero(
    const struct native_handle_close_report *report
)
{
    assert(report != NULL);
    assert(report->attempted_handles == 0U &&
        report->callback_attempts == 0U &&
        report->closed_resources == 0U &&
        report->consumed_error_resources == 0U &&
        report->retained_resources == 0U &&
        report->stale_entries == 0U &&
        report->invalid_entries == 0U &&
        report->invalid_arguments == 0U &&
        report->duplicate_references == 0U &&
        report->retired_handles == 0U &&
        report->omitted_entries == 0U &&
        report->active_handles_before == 0U &&
        report->active_handles_after == 0U &&
        report->active_objects_before == 0U &&
        report->active_objects_after == 0U &&
        !report->retryable && !report->progress && !report->truncated &&
        report->status == NATIVE_HANDLE_OK);
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        assert_summary_zero(&report->types[type]);
    }
}

static void assert_type_summary(
    const struct native_handle_close_report *report,
    uint8_t type,
    uint16_t attempted,
    uint16_t callbacks,
    uint16_t closed,
    uint16_t consumed,
    uint16_t retained,
    uint16_t duplicates,
    uint16_t stale,
    uint16_t invalid,
    uint16_t retired
)
{
    const struct native_handle_close_type_summary *summary =
        summary_for(report, type);

    assert(summary->attempted_handles == attempted);
    assert(summary->callback_attempts == callbacks);
    assert(summary->closed_resources == closed);
    assert(summary->consumed_error_resources == consumed);
    assert(summary->retained_resources == retained);
    assert(summary->duplicate_references == duplicates);
    assert(summary->stale_entries == stale);
    assert(summary->invalid_entries == invalid);
    assert(summary->retired_handles == retired);
}

static void report_argument_and_reset_test(void)
{
    struct native_handle_close_report report;
    struct native_handle_table table = {0};

    clear_bytes(&report, sizeof(report));
    report.attempted_handles = 19U;
    report.types[PHIPIA_HANDLE_FILE].retired_handles = 3U;
    assert(native_handle_close_all_diagnostics(NULL, scripted_close, NULL,
        &report) == NATIVE_HANDLE_NULL_ARGUMENT);
    assert(report.status == NATIVE_HANDLE_NULL_ARGUMENT &&
        report.invalid_arguments == 1U && report.attempted_handles == 0U);
    assert_summary_zero(&report.types[PHIPIA_HANDLE_FILE]);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, NULL,
        &report) == NATIVE_HANDLE_BAD_LIMIT);
    assert(report.status == NATIVE_HANDLE_BAD_LIMIT &&
        report.invalid_arguments == 1U && report.attempted_handles == 0U);
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        assert_summary_zero(&report.types[type]);
    }

    clear_bytes(&report, sizeof(report));
    report.attempted_handles = 4U;
    report.types[PHIPIA_HANDLE_TIMER].callback_attempts = 7U;
    native_handle_close_report_reset(&report);
    assert_report_counters_zero(&report);
    native_handle_close_report_reset(NULL);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, NULL,
        NULL) == NATIVE_HANDLE_NULL_ARGUMENT);
}

static void all_resource_classes_exact_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct close_script script;
    phipia_handle_t handles[sizeof(all_types) / sizeof(all_types[0])];

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    script.results[PHIPIA_HANDLE_DIRECTORY] =
        NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    script.results[PHIPIA_HANDLE_WINDOW] = NATIVE_RESOURCE_RETAINED;
    script.results[PHIPIA_HANDLE_PACKAGE_UPLOAD] =
        NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    assert(native_handle_table_initialize(&table, 12U) == NATIVE_HANDLE_OK);
    for (size_t index = 0U; index < sizeof(all_types) / sizeof(all_types[0]);
         ++index) {
        install_resource(&table, all_types[index],
            UINT64_C(0x1000) + index * UINT64_C(0x10), &handles[index]);
    }
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(report.attempted_handles == 11U && report.callback_attempts == 11U &&
        report.closed_resources == 8U &&
        report.consumed_error_resources == 2U &&
        report.retained_resources == 1U && report.retired_handles == 10U &&
        report.active_handles_before == 11U &&
        report.active_handles_after == 1U && report.active_objects_after == 1U);
    for (size_t index = 0U; index < sizeof(all_types) / sizeof(all_types[0]);
         ++index) {
        const uint8_t type = all_types[index];

        assert_type_summary(&report, type, 1U, 1U,
            type == PHIPIA_HANDLE_DIRECTORY ||
                type == PHIPIA_HANDLE_PACKAGE_UPLOAD ? 0U :
                type == PHIPIA_HANDLE_WINDOW ? 0U : 1U,
            type == PHIPIA_HANDLE_DIRECTORY ||
                type == PHIPIA_HANDLE_PACKAGE_UPLOAD ? 1U : 0U,
            type == PHIPIA_HANDLE_WINDOW ? 1U : 0U,
            0U, 0U, 0U,
            type == PHIPIA_HANDLE_WINDOW ? 0U : 1U);
        assert(script.calls[type] == 1U);
    }
    assert(report.types[PHIPIA_HANDLE_WINDOW].retained_resources == 1U &&
        report.types[PHIPIA_HANDLE_WINDOW].retired_handles == 0U);
    assert(report.types[PHIPIA_HANDLE_FILE].attempted_handles == 1U);
    script.results[PHIPIA_HANDLE_WINDOW] = NATIVE_RESOURCE_CLOSED;
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert(report.active_handles_before == 1U &&
        report.active_handles_after == 0U && report.progress &&
        report.types[PHIPIA_HANDLE_WINDOW].closed_resources == 1U &&
        report.types[PHIPIA_HANDLE_WINDOW].retired_handles == 1U &&
        script.calls[PHIPIA_HANDLE_WINDOW] == 2U);
}

static void duplicate_accounting_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct close_script script;
    phipia_handle_t first;
    phipia_handle_t second;
    phipia_handle_t third;

    script_initialize(&script, NATIVE_RESOURCE_RETAINED);
    assert(native_handle_table_initialize(&table, 3U) == NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_FILE, UINT64_C(31), &first);
    assert(native_handle_duplicate(&table, first, &second) == NATIVE_HANDLE_OK);
    assert(native_handle_duplicate(&table, first, &third) == NATIVE_HANDLE_OK);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(report.attempted_handles == 3U && report.callback_attempts == 1U &&
        report.duplicate_references == 2U &&
        report.retained_resources == 1U && report.retired_handles == 2U &&
        report.progress && report.retryable &&
        report.active_handles_after == 1U);
    assert_type_summary(&report, PHIPIA_HANDLE_FILE, 3U, 1U, 0U, 0U, 1U,
        2U, 0U, 0U, 2U);
    assert(script.calls[PHIPIA_HANDLE_FILE] == 1U);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(report.attempted_handles == 1U && report.callback_attempts == 1U &&
        report.duplicate_references == 0U && report.retained_resources == 1U &&
        !report.progress && report.retryable &&
        report.active_handles_after == 1U);
    assert_type_summary(&report, PHIPIA_HANDLE_FILE, 1U, 1U, 0U, 0U, 1U,
        0U, 0U, 0U, 0U);
    script.results[PHIPIA_HANDLE_FILE] = NATIVE_RESOURCE_CLOSED;
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert(report.progress && !report.retryable &&
        report.types[PHIPIA_HANDLE_FILE].closed_resources == 1U &&
        report.types[PHIPIA_HANDLE_FILE].retired_handles == 1U &&
        script.calls[PHIPIA_HANDLE_FILE] == 3U);
}

static void single_close_accounting_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct close_script script;
    phipia_handle_t first;
    phipia_handle_t duplicate;
    struct native_resource *resolved;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED_WITH_ERROR);
    assert(native_handle_table_initialize(&table, 2U) == NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_FILE, UINT64_C(41), &first);
    assert(native_handle_duplicate(&table, first, &duplicate) ==
        NATIVE_HANDLE_OK);
    assert(native_handle_close_with_report(&table, first, scripted_close,
        &script, &report) == NATIVE_HANDLE_OK);
    assert(report.callback_attempts == 0U && report.duplicate_references == 1U &&
        report.retired_handles == 1U && report.progress &&
        report.types[PHIPIA_HANDLE_FILE].callback_attempts == 0U &&
        report.types[PHIPIA_HANDLE_FILE].duplicate_references == 1U &&
        report.types[PHIPIA_HANDLE_FILE].retired_handles == 1U &&
        script.calls[PHIPIA_HANDLE_FILE] == 0U);
    assert(native_handle_close_with_report(&table, duplicate, scripted_close,
        &script, &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(report.callback_attempts == 1U &&
        report.consumed_error_resources == 1U && report.retired_handles == 1U &&
        report.types[PHIPIA_HANDLE_FILE].consumed_error_resources == 1U &&
        report.types[PHIPIA_HANDLE_FILE].retired_handles == 1U &&
        !report.retryable && script.calls[PHIPIA_HANDLE_FILE] == 1U);
    assert(native_handle_close_with_report(&table, duplicate, scripted_close,
        &script, &report) == NATIVE_HANDLE_STALE);
    assert(report.stale_entries == 1U &&
        report.types[PHIPIA_HANDLE_FILE].stale_entries == 1U &&
        report.types[PHIPIA_HANDLE_FILE].attempted_handles == 1U &&
        script.calls[PHIPIA_HANDLE_FILE] == 1U);
    assert(native_handle_resolve(&table, duplicate, PHIPIA_HANDLE_FILE,
        &resolved) == NATIVE_HANDLE_STALE);
    assert(native_handle_close_with_report(&table, PHIPIA_HANDLE_INVALID,
        scripted_close, &script, &report) == NATIVE_HANDLE_STALE);
    assert(report.invalid_entries == 1U && report.attempted_handles == 1U &&
        report.callback_attempts == 0U);
    assert_summary_zero(&report.types[PHIPIA_HANDLE_FILE]);
}

static void single_close_all_type_outcomes_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct close_script script;
    phipia_handle_t handle;

    for (size_t index = 0U; index < sizeof(all_types) / sizeof(all_types[0]);
         ++index) {
        const uint8_t type = all_types[index];
        const enum native_resource_close_result expected =
            index % 3U == 0U ? NATIVE_RESOURCE_CLOSED :
            index % 3U == 1U ? NATIVE_RESOURCE_CLOSED_WITH_ERROR :
            NATIVE_RESOURCE_RETAINED;

        script_initialize(&script, expected);
        assert(native_handle_table_initialize(&table, 1U) == NATIVE_HANDLE_OK);
        install_resource(&table, type, UINT64_C(0x5000) + index, &handle);
        assert(native_handle_close_with_report(&table, handle, scripted_close,
            &script, &report) == (expected == NATIVE_RESOURCE_CLOSED ?
                NATIVE_HANDLE_OK : NATIVE_HANDLE_CLOSE_FAILED));
        assert(report.attempted_handles == 1U && report.callback_attempts == 1U &&
            report.active_handles_before == 1U &&
            report.active_objects_before == 1U &&
            report.active_handles_after == (expected ==
                NATIVE_RESOURCE_RETAINED ? 1U : 0U) &&
            report.active_objects_after == (expected ==
                NATIVE_RESOURCE_RETAINED ? 1U : 0U) &&
            script.calls[type] == 1U);
        assert_type_summary(&report, type, 1U, 1U,
            expected == NATIVE_RESOURCE_CLOSED ? 1U : 0U,
            expected == NATIVE_RESOURCE_CLOSED_WITH_ERROR ? 1U : 0U,
            expected == NATIVE_RESOURCE_RETAINED ? 1U : 0U,
            0U, 0U, 0U,
            expected == NATIVE_RESOURCE_RETAINED ? 0U : 1U);
        assert(report.retryable == (expected == NATIVE_RESOURCE_RETAINED) &&
            report.progress == (expected != NATIVE_RESOURCE_RETAINED));
        if (expected == NATIVE_RESOURCE_RETAINED) {
            script.results[type] = NATIVE_RESOURCE_CLOSED;
            assert(native_handle_close_with_report(&table, handle,
                scripted_close, &script, &report) == NATIVE_HANDLE_OK);
            assert(report.types[type].closed_resources == 1U &&
                report.types[type].retired_handles == 1U &&
                report.active_handles_after == 0U &&
                script.calls[type] == 2U);
        }
        assert(table.active_handles == 0U && table.active_objects == 0U);
        for (size_t other = 0U; other < NATIVE_HANDLE_CLOSE_TYPE_COUNT;
             ++other) {
            if (other != type) {
                assert_summary_zero(&report.types[other]);
            }
        }
    }
}

static void malformed_entry_accounting_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct close_script script;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    assert(native_handle_table_initialize(&table, 4U) == NATIVE_HANDLE_OK);
    table.active_handles = 4U;
    table.slots[0].active = true;
    table.slots[0].generation = 1U;
    table.slots[0].type = PHIPIA_HANDLE_FILE;
    table.slots[0].object_index = 4U;
    table.slots[1].active = true;
    table.slots[1].generation = 1U;
    table.slots[1].type = PHIPIA_HANDLE_DIRECTORY;
    table.slots[1].object_index = 0U;
    table.slots[2].active = true;
    table.slots[2].generation = 0U;
    table.slots[2].type = PHIPIA_HANDLE_WINDOW;
    table.slots[2].object_index = 0U;
    table.slots[3].active = true;
    table.slots[3].generation = 0U;
    table.slots[3].type = UINT8_C(0xFE);
    table.slots[3].object_index = 0U;
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(report.attempted_handles == 4U && report.callback_attempts == 0U &&
        report.stale_entries == 1U && report.invalid_entries == 3U &&
        report.active_handles_after == 4U && !report.retryable &&
        !report.progress);
    assert_type_summary(&report, PHIPIA_HANDLE_FILE, 1U, 0U, 0U, 0U, 0U,
        0U, 0U, 1U, 0U);
    assert_type_summary(&report, PHIPIA_HANDLE_DIRECTORY, 1U, 0U, 0U, 0U,
        0U, 0U, 1U, 0U, 0U);
    assert_type_summary(&report, PHIPIA_HANDLE_WINDOW, 1U, 0U, 0U, 0U, 0U,
        0U, 0U, 1U, 0U);
    for (size_t type = PHIPIA_HANDLE_EVENT_QUEUE;
         type <= PHIPIA_HANDLE_PACKAGE_CONTROL; ++type) {
        assert_summary_zero(&report.types[type]);
    }
    assert(native_handle_close_all(&table, scripted_close, &script) ==
        NATIVE_HANDLE_CLOSE_FAILED);
    assert(script.order_count == 0U);
}

static void invalid_table_and_unknown_type_test(void)
{
    struct native_handle_close_report report;
    struct native_handle_table table = {0};
    phipia_handle_t unknown = (UINT64_C(1) << 32) |
        ((uint64_t)UINT8_C(0xFE) << 16) | UINT64_C(1);

    clear_bytes(&report, sizeof(report));
    assert(native_handle_close_with_report(&table, unknown, NULL, NULL,
        &report) == NATIVE_HANDLE_BAD_LIMIT);
    assert(report.invalid_arguments == 1U && report.attempted_handles == 0U);
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        assert_summary_zero(&report.types[type]);
    }
    assert(native_handle_close_all_diagnostics(NULL, NULL, NULL, &report) ==
        NATIVE_HANDLE_NULL_ARGUMENT);
    assert(report.invalid_arguments == 1U && report.attempted_handles == 0U);
    native_handle_close_report_reset(&report);
    assert_report_counters_zero(&report);
}

static void bounded_report_type_test(void)
{
    static struct native_handle_table table;
    struct native_handle_close_report report;
    struct close_script script;
    phipia_handle_t handle;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    assert(native_handle_table_initialize(&table, NATIVE_HANDLE_LIMIT) ==
        NATIVE_HANDLE_OK);
    for (size_t index = 0U; index < NATIVE_HANDLE_LIMIT; ++index) {
        install_resource(&table, PHIPIA_HANDLE_TIMER,
            UINT64_C(0x8000) + index, &handle);
    }
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert(report.attempted_handles == NATIVE_HANDLE_LIMIT &&
        report.callback_attempts == NATIVE_HANDLE_LIMIT &&
        report.closed_resources == NATIVE_HANDLE_LIMIT &&
        report.retired_handles == NATIVE_HANDLE_LIMIT &&
        report.omitted_entries == NATIVE_HANDLE_LIMIT -
            NATIVE_HANDLE_CLOSE_REPORT_CAPACITY && report.truncated);
    assert_type_summary(&report, PHIPIA_HANDLE_TIMER, NATIVE_HANDLE_LIMIT,
        NATIVE_HANDLE_LIMIT, NATIVE_HANDLE_LIMIT, 0U, 0U, 0U, 0U, 0U,
        NATIVE_HANDLE_LIMIT);
    assert(script.calls[PHIPIA_HANDLE_TIMER] == NATIVE_HANDLE_LIMIT);
    native_handle_close_report_reset(&report);
    assert_report_counters_zero(&report);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert_report_counters_zero(&report);
}

static void fill_attempt(
    struct native_process_teardown_attempt *attempt,
    uint64_t number
)
{
    assert(attempt != NULL);
    clear_bytes(attempt, sizeof(*attempt));
    attempt->attempt_number = number;
    attempt->handles_attempted = (uint16_t)number;
    attempt->callbacks_attempted = (uint16_t)(number + 1U);
    attempt->closed_resources = (uint16_t)(number + 2U);
    attempt->consumed_error_resources = (uint16_t)(number + 3U);
    attempt->retained_resources = (uint16_t)(number + 4U);
    attempt->duplicate_references = (uint16_t)(number + 5U);
    attempt->stale_entries = (uint16_t)(number + 6U);
    attempt->invalid_entries = (uint16_t)(number + 7U);
    attempt->retired_handles = (uint16_t)(number + 8U);
    attempt->active_handles_before = (uint16_t)(number + 9U);
    attempt->active_handles_after = (uint16_t)(number + 10U);
    attempt->active_objects_before = (uint16_t)(number + 11U);
    attempt->active_objects_after = (uint16_t)(number + 12U);
    attempt->made_progress = (number & 1U) != 0U;
    attempt->retryable = (number & 2U) != 0U;
    attempt->close_failed = (number & 16U) != 0U;
    attempt->blocked = (number & 4U) != 0U;
    attempt->retired = (number & 8U) != 0U;
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        attempt->types[type].attempted_handles = (uint16_t)number;
        attempt->types[type].callback_attempts = (uint16_t)(number + 1U);
        attempt->types[type].closed_resources = (uint16_t)(number + 2U);
        attempt->types[type].consumed_error_resources = (uint16_t)(number + 3U);
        attempt->types[type].retained_resources = (uint16_t)(number + 4U);
        attempt->types[type].duplicate_references = (uint16_t)(number + 5U);
        attempt->types[type].stale_entries = (uint16_t)(number + 6U);
        attempt->types[type].invalid_entries = (uint16_t)(number + 7U);
        attempt->types[type].retired_handles = (uint16_t)(number + 8U);
    }
}

static void assert_attempt_marker(
    const struct native_process_teardown_attempt *attempt,
    uint64_t number
)
{
    assert(attempt != NULL && attempt->attempt_number == number);
    assert(attempt->handles_attempted == number &&
        attempt->callbacks_attempted == number + 1U &&
        attempt->closed_resources == number + 2U &&
        attempt->consumed_error_resources == number + 3U &&
        attempt->retained_resources == number + 4U &&
        attempt->duplicate_references == number + 5U &&
        attempt->stale_entries == number + 6U &&
        attempt->invalid_entries == number + 7U &&
        attempt->retired_handles == number + 8U &&
        attempt->active_handles_before == number + 9U &&
        attempt->active_handles_after == number + 10U &&
        attempt->active_objects_before == number + 11U &&
        attempt->active_objects_after == number + 12U);
    assert(attempt->made_progress == ((number & 1U) != 0U) &&
        attempt->retryable == ((number & 2U) != 0U) &&
        attempt->close_failed == ((number & 16U) != 0U) &&
        attempt->blocked == ((number & 4U) != 0U) &&
        attempt->retired == ((number & 8U) != 0U));
}

static void assert_attempt_type_marker(
    const struct native_process_teardown_attempt *attempt,
    uint64_t number
)
{
    assert(attempt != NULL);
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        const struct native_handle_close_type_summary *summary =
            &attempt->types[type];

        assert(summary->attempted_handles == number &&
            summary->callback_attempts == number + 1U &&
            summary->closed_resources == number + 2U &&
            summary->consumed_error_resources == number + 3U &&
            summary->retained_resources == number + 4U &&
            summary->duplicate_references == number + 5U &&
            summary->stale_entries == number + 6U &&
            summary->invalid_entries == number + 7U &&
            summary->retired_handles == number + 8U);
    }
}

static void history_argument_test(void)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    struct native_process_teardown_attempt output[2];

    native_process_teardown_history_reset(NULL);
    native_process_teardown_history_append(NULL, NULL);
    native_process_teardown_history_reset(&history);
    native_process_teardown_history_append(&history, NULL);
    assert(history.valid_entries == 0U && history.total_attempts == 0U &&
        history.dropped_attempts == 0U && !history.truncated);
    fill_attempt(&attempt, 1U);
    native_process_teardown_history_append(&history, &attempt);
    assert(history.valid_entries == 1U && history.insertion_cursor == 1U &&
        history.total_attempts == 1U && history.dropped_attempts == 0U &&
        !history.truncated);
    assert(native_process_teardown_history_copy(NULL, output, 2U) == 0U);
    assert(native_process_teardown_history_copy(&history, NULL, 2U) == 0U);
    assert(native_process_teardown_history_copy(&history, output, 0U) == 0U);
    clear_bytes(output, sizeof(output));
    assert(native_process_teardown_history_copy(&history, output, 2U) == 1U);
    assert_attempt_marker(&output[0], 1U);
    assert_attempt_type_marker(&output[0], 1U);
}

static void history_ring_capacity_test(void)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    struct native_process_teardown_attempt output[
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY];

    native_process_teardown_history_reset(&history);
    for (uint64_t number = 1U;
         number <= NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY; ++number) {
        fill_attempt(&attempt, number);
        native_process_teardown_history_append(&history, &attempt);
        assert(history.valid_entries == number &&
            history.total_attempts == number && history.dropped_attempts == 0U &&
            !history.truncated);
    }
    assert(history.insertion_cursor == 0U);
    assert(native_process_teardown_history_copy(&history, output,
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY) ==
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY);
    for (size_t index = 0U;
         index < NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY; ++index) {
        assert_attempt_marker(&output[index], index + 1U);
        assert_attempt_type_marker(&output[index], index + 1U);
    }
}

static void history_overflow_and_copy_capacity_test(void)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    struct native_process_teardown_attempt output[8];
    struct native_process_teardown_attempt smaller[3];

    native_process_teardown_history_reset(&history);
    for (uint64_t number = 1U; number <= 11U; ++number) {
        fill_attempt(&attempt, number);
        native_process_teardown_history_append(&history, &attempt);
    }
    assert(history.valid_entries == NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY &&
        history.insertion_cursor == 3U && history.total_attempts == 11U &&
        history.dropped_attempts == 3U && history.truncated);
    assert(native_process_teardown_history_copy(&history, output,
        sizeof(output) / sizeof(output[0])) ==
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY);
    for (size_t index = 0U;
         index < NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY; ++index) {
        assert_attempt_marker(&output[index], index + 4U);
        assert_attempt_type_marker(&output[index], index + 4U);
    }
    assert(native_process_teardown_history_copy(&history, smaller,
        sizeof(smaller) / sizeof(smaller[0])) ==
        sizeof(smaller) / sizeof(smaller[0]));
    for (size_t index = 0U; index < sizeof(smaller) / sizeof(smaller[0]);
         ++index) {
        assert_attempt_marker(&smaller[index], index + 9U);
        assert_attempt_type_marker(&smaller[index], index + 9U);
    }
    assert(native_process_teardown_history_copy(&history, output, 1U) == 1U);
    assert_attempt_marker(&output[0], 11U);
    assert_attempt_type_marker(&output[0], 11U);
}

static void history_uninitialized_shape_test(void)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    struct native_process_teardown_attempt output;

    clear_bytes(&history, sizeof(history));
    history.valid_entries = UINT16_MAX;
    history.insertion_cursor = UINT16_MAX;
    assert(native_process_teardown_history_copy(&history, &output, 1U) ==
        0U);
    fill_attempt(&attempt, 73U);
    native_process_teardown_history_append(&history, &attempt);
    assert(history.valid_entries == 1U && history.insertion_cursor == 1U &&
        history.total_attempts == 1U && history.dropped_attempts == 0U &&
        !history.truncated);
    assert(native_process_teardown_history_copy(&history, &output, 1U) == 1U);
    assert_attempt_marker(&output, 73U);
    assert_attempt_type_marker(&output, 73U);
}

static void history_reset_after_overflow_test(void)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    struct native_process_teardown_attempt output;

    native_process_teardown_history_reset(&history);
    for (uint64_t number = 1U; number <= 20U; ++number) {
        fill_attempt(&attempt, number);
        native_process_teardown_history_append(&history, &attempt);
    }
    assert(history.truncated && history.dropped_attempts == 12U &&
        history.total_attempts == 20U);
    native_process_teardown_history_reset(&history);
    assert(history.valid_entries == 0U && history.insertion_cursor == 0U &&
        history.total_attempts == 0U && history.dropped_attempts == 0U &&
        !history.truncated);
    assert(native_process_teardown_history_copy(&history, &output, 1U) == 0U);
    fill_attempt(&attempt, 1U);
    native_process_teardown_history_append(&history, &attempt);
    assert(history.valid_entries == 1U && history.total_attempts == 1U &&
        history.dropped_attempts == 0U && !history.truncated);
}

static void history_counter_saturation_test(void)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    struct native_process_teardown_attempt output;

    native_process_teardown_history_reset(&history);
    history.valid_entries = NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;
    history.insertion_cursor = 0U;
    history.total_attempts = UINT64_MAX;
    history.dropped_attempts = UINT64_MAX;
    history.truncated = true;
    fill_attempt(&attempt, UINT64_C(0x55));
    native_process_teardown_history_append(&history, &attempt);
    assert(history.valid_entries == NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY &&
        history.insertion_cursor == 1U && history.total_attempts == UINT64_MAX &&
        history.dropped_attempts == UINT64_MAX && history.truncated);
    assert(native_process_teardown_history_copy(&history, &output, 1U) == 1U);
    assert_attempt_marker(&output, UINT64_C(0x55));
    assert_attempt_type_marker(&output, UINT64_C(0x55));
}

static void append_report_attempt(
    struct native_process_teardown_history *history,
    const struct native_handle_close_report *report,
    uint64_t attempt_number
)
{
    struct native_process_teardown_attempt attempt;

    clear_bytes(&attempt, sizeof(attempt));
    attempt.attempt_number = attempt_number;
    attempt.handles_attempted = report->attempted_handles;
    attempt.callbacks_attempted = report->callback_attempts;
    attempt.closed_resources = report->closed_resources;
    attempt.consumed_error_resources = report->consumed_error_resources;
    attempt.retained_resources = report->retained_resources;
    attempt.duplicate_references = report->duplicate_references;
    attempt.stale_entries = report->stale_entries;
    attempt.invalid_entries = report->invalid_entries;
    attempt.retired_handles = report->retired_handles;
    attempt.active_handles_before = report->active_handles_before;
    attempt.active_handles_after = report->active_handles_after;
    attempt.active_objects_before = report->active_objects_before;
    attempt.active_objects_after = report->active_objects_after;
    attempt.made_progress = report->progress;
    attempt.retryable = report->retryable;
    attempt.close_failed = report->status != NATIVE_HANDLE_OK;
    attempt.blocked = report->active_handles_after != 0U;
    attempt.retired = !attempt.blocked;
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        attempt.types[type] = report->types[type];
    }
    native_process_teardown_history_append(history, &attempt);
}

static void history_preserves_report_census_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt output[2];
    struct close_script script;
    phipia_handle_t file;
    phipia_handle_t duplicate;
    phipia_handle_t directory;
    phipia_handle_t window;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    script.results[PHIPIA_HANDLE_DIRECTORY] =
        NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    script.results[PHIPIA_HANDLE_WINDOW] = NATIVE_RESOURCE_RETAINED;
    assert(native_handle_table_initialize(&table, 4U) == NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_FILE, UINT64_C(0x6100), &file);
    assert(native_handle_duplicate(&table, file, &duplicate) ==
        NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_DIRECTORY, UINT64_C(0x6200),
        &directory);
    install_resource(&table, PHIPIA_HANDLE_WINDOW, UINT64_C(0x6300), &window);
    native_process_teardown_history_reset(&history);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    append_report_attempt(&history, &report, 41U);
    assert(native_process_teardown_history_copy(&history, output, 2U) == 1U);
    assert(output[0].attempt_number == 41U &&
        output[0].handles_attempted == report.attempted_handles &&
        output[0].callbacks_attempted == report.callback_attempts &&
        output[0].closed_resources == report.closed_resources &&
        output[0].consumed_error_resources ==
            report.consumed_error_resources &&
        output[0].retained_resources == report.retained_resources &&
        output[0].duplicate_references == report.duplicate_references &&
        output[0].stale_entries == report.stale_entries &&
        output[0].invalid_entries == report.invalid_entries &&
        output[0].retired_handles == report.retired_handles &&
        output[0].active_handles_before == report.active_handles_before &&
        output[0].active_handles_after == report.active_handles_after &&
        output[0].active_objects_before == report.active_objects_before &&
        output[0].active_objects_after == report.active_objects_after &&
        output[0].made_progress == report.progress &&
        output[0].retryable == report.retryable &&
        output[0].close_failed && report.status == NATIVE_HANDLE_CLOSE_FAILED &&
        output[0].blocked && !output[0].retired);
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        assert(output[0].types[type].attempted_handles ==
            report.types[type].attempted_handles &&
            output[0].types[type].callback_attempts ==
                report.types[type].callback_attempts &&
            output[0].types[type].closed_resources ==
                report.types[type].closed_resources &&
            output[0].types[type].consumed_error_resources ==
                report.types[type].consumed_error_resources &&
            output[0].types[type].retained_resources ==
                report.types[type].retained_resources &&
            output[0].types[type].duplicate_references ==
                report.types[type].duplicate_references &&
            output[0].types[type].stale_entries ==
                report.types[type].stale_entries &&
            output[0].types[type].invalid_entries ==
                report.types[type].invalid_entries &&
            output[0].types[type].retired_handles ==
                report.types[type].retired_handles);
    }
    script.results[PHIPIA_HANDLE_WINDOW] = NATIVE_RESOURCE_CLOSED;
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    append_report_attempt(&history, &report, 42U);
    assert(native_process_teardown_history_copy(&history, output, 2U) == 2U);
    assert(output[0].attempt_number == 41U && output[1].attempt_number == 42U &&
        output[1].active_handles_before == 1U &&
        output[1].active_handles_after == 0U && output[1].retired &&
        !output[1].blocked && !output[1].retryable && !output[1].close_failed &&
        output[1].types[PHIPIA_HANDLE_WINDOW].closed_resources == 1U &&
        output[1].types[PHIPIA_HANDLE_WINDOW].retired_handles == 1U);
    assert(table.active_handles == 0U && table.active_objects == 0U &&
        script.calls[PHIPIA_HANDLE_FILE] == 1U &&
        script.calls[PHIPIA_HANDLE_DIRECTORY] == 1U &&
        script.calls[PHIPIA_HANDLE_WINDOW] == 2U);
}

static void empty_table_history_attempt_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt output[2];
    struct close_script script;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    clear_bytes(&table, sizeof(table));
    native_process_teardown_history_reset(&history);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_BAD_LIMIT);
    append_report_attempt(&history, &report, 1U);
    assert(report.invalid_arguments == 1U && report.attempted_handles == 0U &&
        report.active_handles_before == 0U && report.active_handles_after == 0U &&
        history.entries[0].attempt_number == 1U &&
        history.entries[0].active_handles_before == 0U &&
        history.entries[0].active_handles_after == 0U &&
        !history.entries[0].blocked && history.entries[0].retired &&
        !history.entries[0].retryable && history.entries[0].close_failed);
    assert(native_handle_table_initialize(&table, 2U) == NATIVE_HANDLE_OK);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    append_report_attempt(&history, &report, 2U);
    assert(report.status == NATIVE_HANDLE_OK && report.attempted_handles == 0U &&
        report.callback_attempts == 0U && report.active_handles_before == 0U &&
        report.active_handles_after == 0U && !report.progress &&
        !report.retryable && !history.entries[1].close_failed &&
        history.valid_entries == 2U);
    assert(native_process_teardown_history_copy(&history, output, 2U) == 2U);
    assert(output[0].attempt_number == 1U && output[1].attempt_number == 2U &&
        output[0].retired && output[1].retired &&
        output[0].handles_attempted == 0U && output[1].handles_attempted == 0U);
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        assert_summary_zero(&report.types[type]);
        assert_summary_zero(&output[0].types[type]);
        assert_summary_zero(&output[1].types[type]);
    }
}

static void history_copy_after_repeated_partial_reads_test(void)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    struct native_process_teardown_attempt first[4];
    struct native_process_teardown_attempt second[4];
    struct native_process_teardown_attempt newest;

    native_process_teardown_history_reset(&history);
    for (uint64_t number = 1U; number <= 9U; ++number) {
        fill_attempt(&attempt, number);
        native_process_teardown_history_append(&history, &attempt);
    }
    assert(native_process_teardown_history_copy(&history, first,
        sizeof(first) / sizeof(first[0])) == 4U);
    assert(native_process_teardown_history_copy(&history, second,
        sizeof(second) / sizeof(second[0])) == 4U);
    for (size_t index = 0U; index < 4U; ++index) {
        assert_attempt_marker(&first[index], index + 6U);
        assert_attempt_marker(&second[index], index + 6U);
    }
    assert(native_process_teardown_history_copy(&history, &newest, 1U) == 1U);
    assert_attempt_marker(&newest, 9U);
    fill_attempt(&attempt, 10U);
    native_process_teardown_history_append(&history, &attempt);
    assert(native_process_teardown_history_copy(&history, first, 4U) == 4U);
    for (size_t index = 0U; index < 4U; ++index) {
        assert_attempt_marker(&first[index], index + 7U);
    }
    assert(history.valid_entries == NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY &&
        history.total_attempts == 10U && history.dropped_attempts == 2U &&
        history.truncated);
}

static void process_boundary_blocked_retry_retired_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt output[3];
    struct close_script script;
    phipia_handle_t file;
    phipia_handle_t duplicate;
    phipia_handle_t window;

    script_initialize(&script, NATIVE_RESOURCE_RETAINED);
    assert(native_handle_table_initialize(&table, 3U) == NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_FILE, UINT64_C(0xA1), &file);
    assert(native_handle_duplicate(&table, file, &duplicate) ==
        NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_WINDOW, UINT64_C(0xA2), &window);
    native_process_teardown_history_reset(&history);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    append_report_attempt(&history, &report, 1U);
    assert(report.active_handles_before == 3U &&
        report.active_handles_after == 2U && report.active_objects_before == 2U &&
        report.active_objects_after == 2U && report.progress && report.retryable &&
        report.duplicate_references == 1U && report.callback_attempts == 2U &&
        report.retired_handles == 1U && script.calls[PHIPIA_HANDLE_FILE] == 1U &&
        script.calls[PHIPIA_HANDLE_WINDOW] == 1U);
    assert(history.entries[0].blocked && !history.entries[0].retired &&
        history.entries[0].made_progress && history.entries[0].retryable &&
        history.entries[0].close_failed &&
        history.entries[0].active_handles_before == 3U &&
        history.entries[0].active_handles_after == 2U);

    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    append_report_attempt(&history, &report, 2U);
    assert(report.active_handles_before == 2U && report.active_handles_after == 2U &&
        !report.progress && report.retryable && report.duplicate_references == 0U &&
        report.callback_attempts == 2U && script.calls[PHIPIA_HANDLE_FILE] == 2U &&
        script.calls[PHIPIA_HANDLE_WINDOW] == 2U);
    assert(history.entries[1].blocked && !history.entries[1].retired &&
        !history.entries[1].made_progress && history.entries[1].retryable &&
        history.entries[1].close_failed &&
        history.entries[1].active_handles_before == 2U &&
        history.entries[1].active_handles_after == 2U);

    script.results[PHIPIA_HANDLE_FILE] = NATIVE_RESOURCE_CLOSED;
    script.results[PHIPIA_HANDLE_WINDOW] = NATIVE_RESOURCE_CLOSED;
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    append_report_attempt(&history, &report, 3U);
    assert(report.active_handles_before == 2U && report.active_handles_after == 0U &&
        report.closed_resources == 2U && report.retired_handles == 2U &&
        report.progress && !report.retryable &&
        script.calls[PHIPIA_HANDLE_FILE] == 3U &&
        script.calls[PHIPIA_HANDLE_WINDOW] == 3U);
    assert(history.entries[2].retired && !history.entries[2].blocked &&
        history.entries[2].made_progress && !history.entries[2].retryable &&
        !history.entries[2].close_failed &&
        history.entries[2].active_handles_before == 2U &&
        history.entries[2].active_handles_after == 0U);
    assert(native_process_teardown_history_copy(&history, output, 3U) == 3U);
    assert(output[0].attempt_number == 1U && output[1].attempt_number == 2U &&
        output[2].attempt_number == 3U);
    assert(output[0].types[PHIPIA_HANDLE_FILE].duplicate_references == 1U &&
        output[0].types[PHIPIA_HANDLE_FILE].retired_handles == 1U &&
        output[0].types[PHIPIA_HANDLE_WINDOW].retained_resources == 1U &&
        output[2].types[PHIPIA_HANDLE_FILE].closed_resources == 1U &&
        output[2].types[PHIPIA_HANDLE_WINDOW].closed_resources == 1U);
    assert(table.active_handles == 0U && table.active_objects == 0U);
    assert(file != duplicate && window != PHIPIA_HANDLE_INVALID);
}

static void consumed_error_process_attempt_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt output;
    struct close_script script;
    phipia_handle_t handle;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED_WITH_ERROR);
    assert(native_handle_table_initialize(&table, 1U) == NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_TIMER, UINT64_C(0xB1), &handle);
    native_process_teardown_history_reset(&history);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    append_report_attempt(&history, &report, 1U);
    assert(report.status == NATIVE_HANDLE_CLOSE_FAILED &&
        report.consumed_error_resources == 1U && report.retired_handles == 1U &&
        report.active_handles_after == 0U && !report.retryable &&
        report.progress && script.calls[PHIPIA_HANDLE_TIMER] == 1U);
    assert(history.entries[0].consumed_error_resources == 1U &&
        history.entries[0].retired_handles == 1U &&
        history.entries[0].active_handles_after == 0U &&
        !history.entries[0].retryable && history.entries[0].close_failed &&
        !history.entries[0].blocked &&
        history.entries[0].retired);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert(script.calls[PHIPIA_HANDLE_TIMER] == 1U &&
        report.attempted_handles == 0U && report.callback_attempts == 0U);
    assert(native_process_teardown_history_copy(&history, &output, 1U) == 1U);
    assert(output.types[PHIPIA_HANDLE_TIMER].consumed_error_resources == 1U &&
        output.types[PHIPIA_HANDLE_TIMER].retired_handles == 1U);
    assert(table.active_handles == 0U && table.active_objects == 0U &&
        handle != PHIPIA_HANDLE_INVALID);
}

static void process_result_compatibility_test(void)
{
    struct native_process_result result;
    struct native_process_teardown_attempt attempt;

    clear_bytes(&result, sizeof(result));
    fill_attempt(&attempt, 9U);
    native_process_teardown_history_reset(
        &result.teardown_report.history);
    native_process_teardown_history_append(
        &result.teardown_report.history, &attempt);
    result.teardown_report.attempts = 1U;
    result.teardown_report.blocked = true;
    result.teardown_report.retired = false;
    result.teardown_report.handles.status = NATIVE_HANDLE_CLOSE_FAILED;
    result.teardown_report.handles.retryable = true;
    result.teardown_report.handles.active_handles_after = 1U;
    assert(result.teardown_report.attempts == 1U &&
        result.teardown_report.blocked && !result.teardown_report.retired &&
        result.teardown_report.handles.status == NATIVE_HANDLE_CLOSE_FAILED &&
        result.teardown_report.handles.retryable &&
        result.teardown_report.handles.active_handles_after == 1U &&
        result.teardown_report.history.valid_entries == 1U &&
        result.teardown_report.history.entries[0].attempt_number == 9U);
    result.teardown_report.blocked = false;
    result.teardown_report.retired = true;
    result.teardown_report.handles.status = NATIVE_HANDLE_CLOSE_FAILED;
    result.teardown_report.handles.retryable = false;
    result.teardown_report.handles.active_handles_after = 0U;
    result.teardown_report.handles.consumed_error_resources = 1U;
    assert(!result.teardown_report.blocked && result.teardown_report.retired &&
        !result.teardown_report.handles.retryable &&
        result.teardown_report.handles.active_handles_after == 0U &&
        result.teardown_report.handles.consumed_error_resources == 1U &&
        result.teardown_report.history.valid_entries == 1U);
}

int main(void)
{
    report_argument_and_reset_test();
    all_resource_classes_exact_test();
    duplicate_accounting_test();
    single_close_accounting_test();
    single_close_all_type_outcomes_test();
    malformed_entry_accounting_test();
    invalid_table_and_unknown_type_test();
    bounded_report_type_test();
    history_argument_test();
    history_ring_capacity_test();
    history_overflow_and_copy_capacity_test();
    history_uninitialized_shape_test();
    history_reset_after_overflow_test();
    history_counter_saturation_test();
    history_copy_after_repeated_partial_reads_test();
    empty_table_history_attempt_test();
    history_preserves_report_census_test();
    process_boundary_blocked_retry_retired_test();
    consumed_error_process_attempt_test();
    process_result_compatibility_test();
    puts("native teardown history: per-type census, bounded ring, retry boundary, and consumed-error PASS");
    return 0;
}
