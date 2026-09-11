#include <assert.h>
#include <stdio.h>

#include <phipia/native_handle.h>
#include <phipia/native_process.h>
#include <phipia/native_teardown_diagnostics.h>

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
};

static void clear_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void fill_bytes(void *pointer, size_t length, uint8_t value)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = value;
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

static bool contains_text(const char *text, const char *needle)
{
    size_t needle_length = 0U;

    assert(text != NULL && needle != NULL);
    while (needle[needle_length] != '\0') {
        ++needle_length;
    }
    for (size_t start = 0U; text[start] != '\0'; ++start) {
        size_t offset = 0U;

        while (offset < needle_length &&
               text[start + offset] == needle[offset]) {
            ++offset;
        }
        if (offset == needle_length) {
            return true;
        }
    }
    return needle_length == 0U;
}

static size_t text_index(const char *text, const char *needle)
{
    size_t needle_length = 0U;

    assert(text != NULL && needle != NULL);
    while (needle[needle_length] != '\0') {
        ++needle_length;
    }
    for (size_t start = 0U; text[start] != '\0'; ++start) {
        size_t offset = 0U;

        while (offset < needle_length &&
               text[start + offset] == needle[offset]) {
            ++offset;
        }
        if (offset == needle_length) {
            return start;
        }
    }
    return SIZE_MAX;
}

static void assert_in_order(
    const char *text,
    const char *first,
    const char *second
)
{
    assert(text_index(text, first) < text_index(text, second));
}

static void assert_prefix_terminated(
    const char *output,
    size_t capacity
)
{
    assert(output != NULL && capacity != 0U);
    assert(output[capacity - 1U] == '\0');
}

static void copy_report_to_attempt(
    const struct native_handle_close_report *report,
    uint64_t attempt_number,
    struct native_process_teardown_attempt *attempt
)
{
    assert(report != NULL && attempt != NULL);
    clear_bytes(attempt, sizeof(*attempt));
    attempt->attempt_number = attempt_number;
    attempt->handles_attempted = report->attempted_handles;
    attempt->callbacks_attempted = report->callback_attempts;
    attempt->closed_resources = report->closed_resources;
    attempt->consumed_error_resources = report->consumed_error_resources;
    attempt->retained_resources = report->retained_resources;
    attempt->duplicate_references = report->duplicate_references;
    attempt->stale_entries = report->stale_entries;
    attempt->invalid_entries = report->invalid_entries;
    attempt->retired_handles = report->retired_handles;
    attempt->active_handles_before = report->active_handles_before;
    attempt->active_handles_after = report->active_handles_after;
    attempt->active_objects_before = report->active_objects_before;
    attempt->active_objects_after = report->active_objects_after;
    attempt->made_progress = report->progress;
    attempt->retryable = report->retryable;
    attempt->close_failed = report->status != NATIVE_HANDLE_OK;
    attempt->blocked = report->active_handles_after != 0U;
    attempt->retired = !attempt->blocked;
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        attempt->types[type] = report->types[type];
    }
}

static uint32_t all_class_mask(void)
{
    uint32_t mask = 0U;

    for (size_t type = PHIPIA_HANDLE_FILE;
         type <= PHIPIA_HANDLE_PACKAGE_CONTROL; ++type) {
        mask |= NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(type);
    }
    return mask;
}

static void assert_report_format_contract(
    const struct native_handle_close_report *report
)
{
    static char output[20000];
    char capacity_zero = 'x';
    size_t required = 0U;
    size_t small_capacity;
    enum native_teardown_diagnostics_status status;

    assert(native_handle_close_report_format(report, NULL, 0U, &required) ==
        NATIVE_TEARDOWN_DIAGNOSTICS_OK && required > 0U);
    assert(native_handle_close_report_format(report, &capacity_zero, 0U,
        &small_capacity) == NATIVE_TEARDOWN_DIAGNOSTICS_OK &&
        small_capacity == required && capacity_zero == 'x');
    assert(native_handle_close_report_format(report, NULL, 1U, &small_capacity) ==
        NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT && small_capacity == 0U);

    fill_bytes(output, sizeof(output), UINT8_C(0xA5));
    assert(native_handle_close_report_format(report, output, 1U,
        &small_capacity) ==
        NATIVE_TEARDOWN_DIAGNOSTICS_INSUFFICIENT_CAPACITY &&
        small_capacity == required && output[0] == '\0');
    fill_bytes(output, sizeof(output), UINT8_C(0xA5));
    status = native_handle_close_report_format(report, output,
        small_capacity, &required);
    assert(status == NATIVE_TEARDOWN_DIAGNOSTICS_INSUFFICIENT_CAPACITY);
    assert_prefix_terminated(output, small_capacity);
    assert((uint8_t)output[small_capacity] == UINT8_C(0xA5));

    fill_bytes(output, sizeof(output), UINT8_C(0xA5));
    assert(native_handle_close_report_format(report, output, required + 1U,
        &small_capacity) == NATIVE_TEARDOWN_DIAGNOSTICS_OK &&
        small_capacity == required && output[required] == '\0' &&
        (uint8_t)output[required + 1U] == UINT8_C(0xA5));
    assert(contains_text(output, "report{status=close_failed"));
    assert(contains_text(output, "omitted_entries=0"));
    assert(contains_text(output, "truncated=0"));
    assert(contains_text(output, "classes=[file{"));
    assert(contains_text(output, "event_queue{"));
    assert(contains_text(output, "audio_output{"));
    assert(contains_text(output, "package_control{"));
    assert_in_order(output, "file{", "directory{");
    assert_in_order(output, "directory{", "window{");
    assert_in_order(output, "window{", "event_queue{");
    assert_in_order(output, "event_queue{", "stream{");
    assert_in_order(output, "stream{", "datagram{");
    assert_in_order(output, "datagram{", "timer{");
    assert_in_order(output, "timer{", "thread{");
    assert_in_order(output, "thread{", "audio_output{");
    assert_in_order(output, "audio_output{", "package_upload{");
    assert_in_order(output, "package_upload{", "package_control{");
}

static void mixed_report_and_format_test(
    struct native_handle_close_report *saved_report
)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct close_script script;
    phipia_handle_t handles[sizeof(all_types) / sizeof(all_types[0])];
    phipia_handle_t duplicate;
    uint32_t expected_retired = all_class_mask() &
        ~NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_WINDOW);
    uint32_t expected_consumed =
        NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_DIRECTORY) |
        NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_PACKAGE_UPLOAD);

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
    assert(native_handle_duplicate(&table, handles[0], &duplicate) ==
        NATIVE_HANDLE_OK);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(native_handle_close_report_validate(&report));
    assert(report.attempted_handles == 12U && report.callback_attempts == 11U &&
        report.closed_resources == 8U &&
        report.consumed_error_resources == 2U &&
        report.retained_resources == 1U && report.duplicate_references == 1U &&
        report.retired_handles == 11U && report.active_handles_before == 12U &&
        report.active_handles_after == 1U && report.active_objects_before == 11U &&
        report.active_objects_after == 1U && report.retryable &&
        report.progress && !report.truncated);
    assert(script.calls[PHIPIA_HANDLE_FILE] == 1U);
    assert(native_handle_close_report_retained_class_mask(&report) ==
        NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_WINDOW));
    assert(native_handle_close_report_consumed_error_class_mask(&report) ==
        expected_consumed);
    assert(native_handle_close_report_retired_class_mask(&report) ==
        expected_retired);
    for (size_t type = PHIPIA_HANDLE_FILE;
         type <= PHIPIA_HANDLE_PACKAGE_CONTROL; ++type) {
        const struct native_handle_close_type_summary *summary =
            &report.types[type];

        assert(summary->attempted_handles ==
            (type == PHIPIA_HANDLE_FILE ? 2U : 1U));
        assert(summary->callback_attempts ==
            (type == PHIPIA_HANDLE_FILE ? 1U : 1U));
        assert(summary->closed_resources ==
            (type == PHIPIA_HANDLE_WINDOW ||
             type == PHIPIA_HANDLE_DIRECTORY ||
             type == PHIPIA_HANDLE_PACKAGE_UPLOAD ? 0U : 1U));
        assert(summary->consumed_error_resources ==
            (type == PHIPIA_HANDLE_DIRECTORY ||
             type == PHIPIA_HANDLE_PACKAGE_UPLOAD ? 1U : 0U));
        assert(summary->retained_resources ==
            (type == PHIPIA_HANDLE_WINDOW ? 1U : 0U));
        assert(summary->duplicate_references ==
            (type == PHIPIA_HANDLE_FILE ? 1U : 0U));
        assert(summary->stale_entries == 0U && summary->invalid_entries == 0U);
        assert(summary->retired_handles ==
            (type == PHIPIA_HANDLE_WINDOW ? 0U :
             type == PHIPIA_HANDLE_FILE ? 2U : 1U));
    }
    assert_report_format_contract(&report);
    assert(saved_report != NULL);
    *saved_report = report;
    native_handle_close_report_reset(&report);
    assert(native_handle_close_report_validate(&report));
    assert(native_handle_close_report_retained_class_mask(&report) == 0U);
    assert(native_handle_close_report_format(&report, NULL, 0U, NULL) ==
        NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT);
    (void)duplicate;
}

static void stale_invalid_and_unknown_type_test(void)
{
    struct native_handle_table table;
    struct native_handle_close_report report;
    struct close_script script;
    static char output[20000];
    size_t required;

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
    assert(native_handle_close_report_validate(&report));
    assert(report.stale_entries == 1U && report.invalid_entries == 2U &&
        report.types[PHIPIA_HANDLE_FILE].invalid_entries == 1U &&
        report.types[PHIPIA_HANDLE_DIRECTORY].stale_entries == 1U);
    assert(native_handle_close_report_format(&report, NULL, 0U, &required) ==
        NATIVE_TEARDOWN_DIAGNOSTICS_OK);
    assert(required < sizeof(output));
    assert(native_handle_close_report_format(&report, output, sizeof(output),
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_OK);
    assert(contains_text(output, "type=unknown(254)"));
    assert(!contains_text(output, "unknown(254){"));
    assert(native_handle_close_report_format(&report, output, sizeof(output),
        NULL) == NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT && output[0] == '\0');
    clear_bytes(&report, sizeof(report));
    report.status = (enum native_handle_status)UINT8_C(0xFF);
    assert(!native_handle_close_report_validate(&report));
    assert(native_handle_close_report_format(&report, output, sizeof(output),
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_INVALID &&
        required == 0U && output[0] == '\0');
}

static void report_entry_truncation_test(void)
{
    static struct native_handle_table table;
    static char output[20000];
    struct native_handle_close_report report;
    struct close_script script;
    phipia_handle_t handle;
    size_t required;

    script_initialize(&script, NATIVE_RESOURCE_CLOSED);
    assert(native_handle_table_initialize(&table, NATIVE_HANDLE_LIMIT) ==
        NATIVE_HANDLE_OK);
    for (size_t index = 0U; index < NATIVE_HANDLE_LIMIT; ++index) {
        install_resource(&table, PHIPIA_HANDLE_TIMER,
            UINT64_C(0x5000) + index, &handle);
    }
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &report) == NATIVE_HANDLE_OK);
    assert(native_handle_close_report_validate(&report));
    assert(report.attempted_handles == NATIVE_HANDLE_LIMIT &&
        report.closed_resources == NATIVE_HANDLE_LIMIT &&
        report.omitted_entries == NATIVE_HANDLE_LIMIT -
            NATIVE_HANDLE_CLOSE_REPORT_CAPACITY && report.truncated &&
        report.types[PHIPIA_HANDLE_TIMER].closed_resources ==
            NATIVE_HANDLE_LIMIT);
    assert(native_handle_close_report_format(&report, NULL, 0U, &required) ==
        NATIVE_TEARDOWN_DIAGNOSTICS_OK);
    assert(required < sizeof(output));
    assert(native_handle_close_report_format(&report, output, sizeof(output),
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_OK);
    assert(contains_text(output, "omitted_entries=96"));
    assert(contains_text(output, "truncated=1"));
    native_handle_close_report_reset(&report);
    assert(native_handle_close_report_validate(&report));
    assert(report.omitted_entries == 0U && !report.truncated &&
        report.attempted_handles == 0U);
}

static void malformed_report_and_argument_test(void)
{
    struct native_handle_close_report report;
    static char output[20000];
    size_t required = 99U;

    fill_bytes(&report, sizeof(report), UINT8_C(0xA5));
    assert(!native_handle_close_report_validate(&report));
    assert(native_handle_close_report_format(NULL, output, sizeof(output),
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT &&
        required == 0U && output[0] == '\0');
    clear_bytes(&report, sizeof(report));
    assert(native_handle_close_report_format(&report, output, sizeof(output),
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_OK && required > 0U);
    report.attempted_handles = 1U;
    assert(!native_handle_close_report_validate(&report));
    assert(native_handle_close_report_format(&report, output, sizeof(output),
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_INVALID &&
        required == 0U && output[0] == '\0');
    assert(native_handle_close_report_format(&report, output, sizeof(output),
        NULL) == NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT && output[0] == '\0');
}

static void history_format_contract(
    const struct native_process_teardown_history *history
)
{
    static char output[30000];
    char capacity_zero = 'x';
    size_t required = 0U;
    size_t small_capacity;
    size_t reported_length;

    assert(native_process_teardown_history_format(history, NULL, 0U,
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_OK && required > 0U);
    assert(native_process_teardown_history_format(history, &capacity_zero, 0U,
        &small_capacity) == NATIVE_TEARDOWN_DIAGNOSTICS_OK &&
        small_capacity == required && capacity_zero == 'x');
    assert(native_process_teardown_history_format(history, NULL, 1U,
        &small_capacity) == NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT &&
        small_capacity == 0U);
    fill_bytes(output, sizeof(output), UINT8_C(0xA5));
    assert(native_process_teardown_history_format(history, output, 1U,
        &small_capacity) ==
        NATIVE_TEARDOWN_DIAGNOSTICS_INSUFFICIENT_CAPACITY &&
        small_capacity == required && output[0] == '\0');
    small_capacity = required / 2U;
    if (small_capacity == 0U) {
        small_capacity = 1U;
    }
    fill_bytes(output, sizeof(output), UINT8_C(0xA5));
    small_capacity = required / 2U;
    if (small_capacity == 0U) {
        small_capacity = 1U;
    }
    assert(native_process_teardown_history_format(history, output,
        small_capacity, &reported_length) ==
        NATIVE_TEARDOWN_DIAGNOSTICS_INSUFFICIENT_CAPACITY &&
        reported_length == required);
    assert_prefix_terminated(output, small_capacity);
    assert((uint8_t)output[small_capacity] == UINT8_C(0xA5));
    fill_bytes(output, sizeof(output), UINT8_C(0xA5));
    assert(native_process_teardown_history_format(history, output, required + 1U,
        &small_capacity) == NATIVE_TEARDOWN_DIAGNOSTICS_OK &&
        small_capacity == required && output[required] == '\0' &&
        (uint8_t)output[required + 1U] == UINT8_C(0xA5));
    assert(contains_text(output, "history{total_attempts=10"));
    assert(contains_text(output, "valid_entries=8"));
    assert(contains_text(output, "dropped_attempts=2"));
    assert(contains_text(output, "truncated=1"));
    assert(contains_text(output, "blocked=1"));
    assert(contains_text(output, "retired=0"));
    assert_in_order(output, "attempt{number=3", "attempt{number=10");
    assert_in_order(output, "attempt{number=3", "file{");
    assert_in_order(output, "file{", "package_control{");
}

static void history_ring_and_derived_test(
    const struct native_handle_close_report *report
)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    struct native_process_teardown_attempt output[8];
    struct native_process_teardown_attempt smaller[3];
    struct native_process_teardown_attempt oldest;
    struct native_process_teardown_attempt latest;
    uint32_t expected_retired = all_class_mask() &
        ~NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_WINDOW);
    uint32_t expected_consumed =
        NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_DIRECTORY) |
        NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_PACKAGE_UPLOAD);

    assert(report != NULL);
    native_process_teardown_history_reset(NULL);
    assert(!native_process_teardown_history_validate(NULL));
    native_process_teardown_history_reset(&history);
    assert(native_process_teardown_history_validate(&history));
    assert(!native_process_teardown_history_latest_attempt(&history, &latest));
    assert(!native_process_teardown_history_oldest_attempt(&history, &oldest));
    assert(!native_process_teardown_history_newest_attempt(&history, &latest));
    assert(!native_process_teardown_history_latest_attempt(NULL, &latest));
    assert(!native_process_teardown_history_latest_attempt(&history, NULL));
    for (uint64_t number = 1U; number <= 10U; ++number) {
        copy_report_to_attempt(report, number, &attempt);
        native_process_teardown_history_append(&history, &attempt);
    }
    assert(native_process_teardown_history_validate(&history));
    assert(history.valid_entries == NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY &&
        history.insertion_cursor == 2U && history.total_attempts == 10U &&
        history.dropped_attempts == 2U && history.truncated);
    assert(native_process_teardown_history_copy(&history, output,
        sizeof(output) / sizeof(output[0])) == 8U);
    for (size_t index = 0U; index < 8U; ++index) {
        assert(output[index].attempt_number == index + 3U);
    }
    assert(native_process_teardown_history_copy(&history, smaller,
        sizeof(smaller) / sizeof(smaller[0])) == 3U);
    assert(smaller[0].attempt_number == 8U &&
        smaller[1].attempt_number == 9U && smaller[2].attempt_number == 10U);
    assert(native_process_teardown_history_copy(&history, NULL, 3U) == 0U);
    assert(native_process_teardown_history_copy(&history, output, 0U) == 0U);
    assert(native_process_teardown_history_oldest_attempt(&history, &oldest) &&
        oldest.attempt_number == 3U);
    assert(native_process_teardown_history_latest_attempt(&history, &latest) &&
        latest.attempt_number == 10U);
    assert(native_process_teardown_history_newest_attempt(&history, &output[0]) &&
        output[0].attempt_number == 10U);
    assert(native_process_teardown_history_retained_class_mask(&history) ==
        NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_WINDOW));
    assert(native_process_teardown_history_blocking_class_mask(&history) ==
        NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(PHIPIA_HANDLE_WINDOW));
    assert(native_process_teardown_history_consumed_error_class_mask(&history) ==
        expected_consumed);
    assert(native_process_teardown_history_retired_class_mask(&history) ==
        expected_retired);
    history_format_contract(&history);

    history.total_attempts = UINT64_MAX;
    history.dropped_attempts = UINT64_MAX;
    history.truncated = true;
    assert(native_process_teardown_history_validate(&history));
    copy_report_to_attempt(report, UINT64_C(99), &attempt);
    native_process_teardown_history_append(&history, &attempt);
    assert(native_process_teardown_history_validate(&history));
    assert(history.total_attempts == UINT64_MAX &&
        history.dropped_attempts == UINT64_MAX && history.truncated &&
        history.entries[(history.insertion_cursor +
            NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY - 1U) %
            NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY].attempt_number == 99U);
    native_process_teardown_history_reset(&history);
    assert(native_process_teardown_history_validate(&history));
    assert(native_process_teardown_history_copy(&history, output, 8U) == 0U);
}

static void malformed_history_test(void)
{
    struct native_process_teardown_history history;
    struct native_process_teardown_attempt attempt;
    static char output[20000];
    size_t required = 77U;

    fill_bytes(&history, sizeof(history), UINT8_C(0xA5));
    assert(!native_process_teardown_history_validate(&history));
    assert(native_process_teardown_history_format(NULL, output, sizeof(output),
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT &&
        required == 0U && output[0] == '\0');
    clear_bytes(&history, sizeof(history));
    history.valid_entries = 1U;
    assert(!native_process_teardown_history_validate(&history));
    assert(native_process_teardown_history_format(&history, output, sizeof(output),
        &required) == NATIVE_TEARDOWN_DIAGNOSTICS_INVALID &&
        required == 0U && output[0] == '\0');
    clear_bytes(&history, sizeof(history));
    history.insertion_cursor = NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;
    assert(!native_process_teardown_history_validate(&history));
    clear_bytes(&history, sizeof(history));
    history.valid_entries = NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY + 1U;
    assert(!native_process_teardown_history_validate(&history));
    clear_bytes(&history, sizeof(history));
    history.valid_entries = NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;
    history.total_attempts = NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;
    history.insertion_cursor = 0U;
    clear_bytes(&attempt, sizeof(attempt));
    history.entries[0] = attempt;
    assert(!native_process_teardown_history_validate(&history));
    assert(native_process_teardown_history_format(&history, output, sizeof(output),
        NULL) == NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT && output[0] == '\0');
}

int main(void)
{
    struct native_handle_table table;
    struct native_handle_close_report mixed_report;
    struct native_handle_close_report retained_report;
    struct close_script script;
    phipia_handle_t handle;

    mixed_report_and_format_test(&mixed_report);
    stale_invalid_and_unknown_type_test();
    report_entry_truncation_test();
    malformed_report_and_argument_test();

    script_initialize(&script, NATIVE_RESOURCE_RETAINED);
    assert(native_handle_table_initialize(&table, 1U) == NATIVE_HANDLE_OK);
    install_resource(&table, PHIPIA_HANDLE_TIMER, UINT64_C(0xB1), &handle);
    assert(native_handle_close_all_diagnostics(&table, scripted_close, &script,
        &retained_report) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(native_handle_close_report_validate(&retained_report) &&
        retained_report.retryable && retained_report.retained_resources == 1U &&
        retained_report.progress == false);
    history_ring_and_derived_test(&mixed_report);
    malformed_history_test();
    assert(handle != PHIPIA_HANDLE_INVALID);
    puts("native teardown diagnostics: validation, masks, bounded formatting, and ring history PASS");
    return 0;
}
