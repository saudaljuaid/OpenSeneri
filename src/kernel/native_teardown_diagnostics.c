/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded validation and text formatting for native teardown diagnostics. */

#include <phipia/native_teardown_diagnostics.h>

struct diagnostic_writer {
    char *output;
    size_t capacity;
    size_t stored;
    size_t required;
};

static bool valid_resource_type(uint8_t type)
{
    return type >= PHIPIA_HANDLE_FILE &&
        type <= PHIPIA_HANDLE_PACKAGE_CONTROL;
}

static bool valid_diagnostic_bool(bool value)
{
    return value == false || value == true;
}

static uint32_t summary_outcome_total(
    const struct native_handle_close_type_summary *summary
)
{
    return (uint32_t)summary->closed_resources +
        (uint32_t)summary->consumed_error_resources +
        (uint32_t)summary->retained_resources +
        (uint32_t)summary->duplicate_references +
        (uint32_t)summary->stale_entries +
        (uint32_t)summary->invalid_entries;
}

static uint32_t report_outcome_total(
    const struct native_handle_close_report *report
)
{
    return (uint32_t)report->closed_resources +
        (uint32_t)report->consumed_error_resources +
        (uint32_t)report->retained_resources +
        (uint32_t)report->duplicate_references +
        (uint32_t)report->stale_entries +
        (uint32_t)report->invalid_entries;
}

static bool summary_is_zero(
    const struct native_handle_close_type_summary *summary
)
{
    return summary != NULL && summary->attempted_handles == 0U &&
        summary->callback_attempts == 0U &&
        summary->closed_resources == 0U &&
        summary->consumed_error_resources == 0U &&
        summary->retained_resources == 0U &&
        summary->duplicate_references == 0U &&
        summary->stale_entries == 0U &&
        summary->invalid_entries == 0U &&
        summary->retired_handles == 0U;
}

static bool summary_validate(
    const struct native_handle_close_type_summary *summary
)
{
    if (summary == NULL || summary->attempted_handles > NATIVE_HANDLE_LIMIT ||
        summary->callback_attempts > NATIVE_HANDLE_LIMIT ||
        summary->closed_resources > NATIVE_HANDLE_LIMIT ||
        summary->consumed_error_resources > NATIVE_HANDLE_LIMIT ||
        summary->retained_resources > NATIVE_HANDLE_LIMIT ||
        summary->duplicate_references > NATIVE_HANDLE_LIMIT ||
        summary->stale_entries > NATIVE_HANDLE_LIMIT ||
        summary->invalid_entries > NATIVE_HANDLE_LIMIT ||
        summary->retired_handles > NATIVE_HANDLE_LIMIT) {
        return false;
    }
    if (summary_outcome_total(summary) != summary->attempted_handles ||
        summary->callback_attempts >
            summary->attempted_handles - summary->duplicate_references ||
        summary->retired_handles !=
            (uint32_t)summary->closed_resources +
            (uint32_t)summary->consumed_error_resources +
            (uint32_t)summary->duplicate_references) {
        return false;
    }
    return true;
}

static void summary_clear(struct native_handle_close_type_summary *summary)
{
    summary->attempted_handles = 0U;
    summary->callback_attempts = 0U;
    summary->closed_resources = 0U;
    summary->consumed_error_resources = 0U;
    summary->retained_resources = 0U;
    summary->duplicate_references = 0U;
    summary->stale_entries = 0U;
    summary->invalid_entries = 0U;
    summary->retired_handles = 0U;
}

static bool entry_is_zero(
    const struct native_handle_close_report_entry *entry
)
{
    return entry != NULL && entry->handle == PHIPIA_HANDLE_INVALID &&
        entry->object_index == 0U && entry->references == 0U &&
        entry->type == 0U && entry->outcome == 0U;
}

static bool close_entry_handle_encoding_valid(
    const struct native_handle_close_report_entry *entry
)
{
    const uint64_t encoded_index = entry->handle & UINT64_C(0xFFFF);
    const uint8_t encoded_type = (uint8_t)(entry->handle >> 16U);
    const uint8_t reserved = (uint8_t)(entry->handle >> 24U);
    const uint32_t generation = (uint32_t)(entry->handle >> 32U);

    return entry->handle != PHIPIA_HANDLE_INVALID && encoded_index != 0U &&
        encoded_index <= NATIVE_HANDLE_LIMIT &&
        encoded_type == entry->type && reserved == 0U && generation != 0U;
}

static bool close_entry_validate(
    const struct native_handle_close_report_entry *entry
)
{
    if (entry == NULL || (entry->object_index != UINT16_MAX &&
            entry->object_index >= NATIVE_HANDLE_LIMIT) ||
        entry->references > NATIVE_HANDLE_LIMIT ||
        entry->outcome >= NATIVE_HANDLE_CLOSE_OUTCOME_COUNT) {
        return false;
    }
    if (!valid_resource_type(entry->type) &&
        entry->outcome != NATIVE_HANDLE_CLOSE_OUTCOME_INVALID) {
        return false;
    }
    switch ((enum native_handle_close_outcome)entry->outcome) {
    case NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED:
    case NATIVE_HANDLE_CLOSE_OUTCOME_CONSUMED_ERROR:
    case NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED:
        return valid_resource_type(entry->type) &&
            close_entry_handle_encoding_valid(entry) &&
            entry->object_index < NATIVE_HANDLE_LIMIT &&
            entry->references != 0U;
    case NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE:
        return valid_resource_type(entry->type) &&
            close_entry_handle_encoding_valid(entry) &&
            entry->object_index < NATIVE_HANDLE_LIMIT &&
            entry->references > 1U;
    case NATIVE_HANDLE_CLOSE_OUTCOME_STALE:
        return (valid_resource_type(entry->type) &&
            close_entry_handle_encoding_valid(entry)) || entry->type == 0U;
    case NATIVE_HANDLE_CLOSE_OUTCOME_INVALID:
        return true;
    default:
        return false;
    }
}

static void summary_record_entry(
    struct native_handle_close_type_summary *summary,
    const struct native_handle_close_report_entry *entry
)
{
    if (!valid_resource_type(entry->type)) {
        return;
    }
    ++summary->attempted_handles;
    switch ((enum native_handle_close_outcome)entry->outcome) {
    case NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED:
        ++summary->callback_attempts;
        ++summary->closed_resources;
        ++summary->retired_handles;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_CONSUMED_ERROR:
        ++summary->callback_attempts;
        ++summary->consumed_error_resources;
        ++summary->retired_handles;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED:
        ++summary->callback_attempts;
        ++summary->retained_resources;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE:
        ++summary->duplicate_references;
        ++summary->retired_handles;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_STALE:
        ++summary->stale_entries;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_INVALID:
        ++summary->invalid_entries;
        break;
    default:
        break;
    }
}

static bool summaries_equal(
    const struct native_handle_close_type_summary *left,
    const struct native_handle_close_type_summary *right
)
{
    return left->attempted_handles == right->attempted_handles &&
        left->callback_attempts == right->callback_attempts &&
        left->closed_resources == right->closed_resources &&
        left->consumed_error_resources == right->consumed_error_resources &&
        left->retained_resources == right->retained_resources &&
        left->duplicate_references == right->duplicate_references &&
        left->stale_entries == right->stale_entries &&
        left->invalid_entries == right->invalid_entries &&
        left->retired_handles == right->retired_handles;
}

static bool report_entries_validate(
    const struct native_handle_close_report *report,
    size_t stored_count
)
{
    struct native_handle_close_type_summary observed[
        NATIVE_HANDLE_CLOSE_TYPE_COUNT];
    uint16_t observed_outcomes[NATIVE_HANDLE_CLOSE_OUTCOME_COUNT] = {0};

    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        summary_clear(&observed[type]);
    }
    for (size_t index = 0U; index < NATIVE_HANDLE_CLOSE_REPORT_CAPACITY;
         ++index) {
        const struct native_handle_close_report_entry *entry =
            &report->entries[index];

        if (index >= stored_count) {
            if (!entry_is_zero(entry)) {
                return false;
            }
            continue;
        }
        if (!close_entry_validate(entry)) {
            return false;
        }
        ++observed_outcomes[entry->outcome];
        if (valid_resource_type(entry->type)) {
            summary_record_entry(&observed[entry->type], entry);
        }
    }
    if (stored_count != report->attempted_handles) {
        return true;
    }
    for (size_t type = PHIPIA_HANDLE_FILE;
         type <= PHIPIA_HANDLE_PACKAGE_CONTROL; ++type) {
        if (!summaries_equal(&observed[type], &report->types[type])) {
            return false;
        }
    }
    return observed_outcomes[NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED] ==
            report->closed_resources &&
        observed_outcomes[NATIVE_HANDLE_CLOSE_OUTCOME_CONSUMED_ERROR] ==
            report->consumed_error_resources &&
        observed_outcomes[NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED] ==
            report->retained_resources &&
        observed_outcomes[NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE] ==
            report->duplicate_references &&
        observed_outcomes[NATIVE_HANDLE_CLOSE_OUTCOME_STALE] ==
            report->stale_entries &&
        observed_outcomes[NATIVE_HANDLE_CLOSE_OUTCOME_INVALID] ==
            report->invalid_entries;
}

bool native_handle_close_report_validate(
    const struct native_handle_close_report *report
)
{
    uint32_t typed_attempts = 0U;
    uint32_t typed_callbacks = 0U;
    uint32_t typed_closed = 0U;
    uint32_t typed_consumed = 0U;
    uint32_t typed_retained = 0U;
    uint32_t typed_duplicates = 0U;
    uint32_t typed_stale = 0U;
    uint32_t typed_invalid = 0U;
    uint32_t typed_retired = 0U;
    size_t stored_count;

    if (report == NULL || report->status < NATIVE_HANDLE_OK ||
        report->status >= NATIVE_HANDLE_STATUS_COUNT ||
        !valid_diagnostic_bool(report->retryable) ||
        !valid_diagnostic_bool(report->progress) ||
        !valid_diagnostic_bool(report->truncated) ||
        report->attempted_handles > NATIVE_HANDLE_LIMIT ||
        report->callback_attempts > NATIVE_HANDLE_LIMIT ||
        report->closed_resources > NATIVE_HANDLE_LIMIT ||
        report->consumed_error_resources > NATIVE_HANDLE_LIMIT ||
        report->retained_resources > NATIVE_HANDLE_LIMIT ||
        report->stale_entries > NATIVE_HANDLE_LIMIT ||
        report->invalid_entries > NATIVE_HANDLE_LIMIT ||
        report->invalid_arguments > 1U ||
        report->duplicate_references > NATIVE_HANDLE_LIMIT ||
        report->retired_handles > NATIVE_HANDLE_LIMIT ||
        report->omitted_entries > NATIVE_HANDLE_LIMIT ||
        report->active_handles_before > NATIVE_HANDLE_LIMIT ||
        report->active_handles_after > NATIVE_HANDLE_LIMIT ||
        report->active_objects_before > NATIVE_HANDLE_LIMIT ||
        report->active_objects_after > NATIVE_HANDLE_LIMIT) {
        return false;
    }
    if (report_outcome_total(report) != report->attempted_handles ||
        report->callback_attempts >
            report->attempted_handles - report->duplicate_references ||
        report->retired_handles !=
            (uint32_t)report->closed_resources +
            (uint32_t)report->consumed_error_resources +
            (uint32_t)report->duplicate_references ||
        report->active_handles_after > report->active_handles_before ||
        report->active_handles_before - report->active_handles_after !=
            report->retired_handles ||
        report->active_objects_after > report->active_objects_before ||
        (uint32_t)report->active_objects_before -
            (uint32_t)report->active_objects_after !=
            (uint32_t)report->closed_resources +
            (uint32_t)report->consumed_error_resources ||
        report->retryable != (report->retained_resources != 0U) ||
        report->progress != (report->retired_handles != 0U)) {
        return false;
    }
    stored_count = report->attempted_handles <
        NATIVE_HANDLE_CLOSE_REPORT_CAPACITY ? report->attempted_handles :
        NATIVE_HANDLE_CLOSE_REPORT_CAPACITY;
    if (report->attempted_handles <= NATIVE_HANDLE_CLOSE_REPORT_CAPACITY) {
        if (report->omitted_entries != 0U || report->truncated) {
            return false;
        }
    } else if (report->omitted_entries != report->attempted_handles -
            NATIVE_HANDLE_CLOSE_REPORT_CAPACITY || !report->truncated) {
        return false;
    }
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        const struct native_handle_close_type_summary *summary =
            &report->types[type];

        if (!summary_validate(summary)) {
            return false;
        }
        if (type == 0U && !summary_is_zero(summary)) {
            return false;
        }
        typed_attempts += summary->attempted_handles;
        typed_callbacks += summary->callback_attempts;
        typed_closed += summary->closed_resources;
        typed_consumed += summary->consumed_error_resources;
        typed_retained += summary->retained_resources;
        typed_duplicates += summary->duplicate_references;
        typed_stale += summary->stale_entries;
        typed_invalid += summary->invalid_entries;
        typed_retired += summary->retired_handles;
    }
    if (typed_attempts > report->attempted_handles ||
        typed_callbacks != report->callback_attempts ||
        typed_closed != report->closed_resources ||
        typed_consumed != report->consumed_error_resources ||
        typed_retained != report->retained_resources ||
        typed_duplicates != report->duplicate_references ||
        typed_stale != report->stale_entries ||
        typed_invalid > report->invalid_entries ||
        typed_retired != report->retired_handles ||
        !report_entries_validate(report, stored_count)) {
        return false;
    }
    if (report->invalid_arguments != 0U) {
        return report->invalid_arguments == 1U &&
            (report->status == NATIVE_HANDLE_NULL_ARGUMENT ||
             report->status == NATIVE_HANDLE_BAD_LIMIT) &&
            report->attempted_handles == 0U &&
            report->active_handles_before == 0U &&
            report->active_handles_after == 0U &&
            report->active_objects_before == 0U &&
            report->active_objects_after == 0U;
    }
    if (report->status == NATIVE_HANDLE_NULL_ARGUMENT ||
        report->status == NATIVE_HANDLE_BAD_LIMIT) {
        return false;
    }
    if (report->status == NATIVE_HANDLE_OK) {
        return report->consumed_error_resources == 0U &&
            report->retained_resources == 0U &&
            report->stale_entries == 0U && report->invalid_entries == 0U;
    }
    if (report->status == NATIVE_HANDLE_STALE) {
        return report->attempted_handles == 1U &&
            report->callback_attempts == 0U &&
            report->closed_resources == 0U &&
            report->consumed_error_resources == 0U &&
            report->retained_resources == 0U &&
            report->duplicate_references == 0U &&
            report->retired_handles == 0U &&
            report->stale_entries + report->invalid_entries == 1U;
    }
    if (report->status == NATIVE_HANDLE_CLOSE_FAILED) {
        return report->retained_resources != 0U ||
            report->consumed_error_resources != 0U ||
            report->stale_entries != 0U || report->invalid_entries != 0U;
    }
    return false;
}

static bool attempt_validate(
    const struct native_process_teardown_attempt *attempt
)
{
    uint32_t typed_attempts = 0U;
    uint32_t typed_callbacks = 0U;
    uint32_t typed_closed = 0U;
    uint32_t typed_consumed = 0U;
    uint32_t typed_retained = 0U;
    uint32_t typed_duplicates = 0U;
    uint32_t typed_stale = 0U;
    uint32_t typed_invalid = 0U;
    uint32_t typed_retired = 0U;

    if (attempt == NULL || attempt->attempt_number == 0U ||
        attempt->handles_attempted > NATIVE_HANDLE_LIMIT ||
        attempt->callbacks_attempted > NATIVE_HANDLE_LIMIT ||
        attempt->closed_resources > NATIVE_HANDLE_LIMIT ||
        attempt->consumed_error_resources > NATIVE_HANDLE_LIMIT ||
        attempt->retained_resources > NATIVE_HANDLE_LIMIT ||
        attempt->duplicate_references > NATIVE_HANDLE_LIMIT ||
        attempt->stale_entries > NATIVE_HANDLE_LIMIT ||
        attempt->invalid_entries > NATIVE_HANDLE_LIMIT ||
        attempt->retired_handles > NATIVE_HANDLE_LIMIT ||
        attempt->active_handles_before > NATIVE_HANDLE_LIMIT ||
        attempt->active_handles_after > NATIVE_HANDLE_LIMIT ||
        attempt->active_objects_before > NATIVE_HANDLE_LIMIT ||
        attempt->active_objects_after > NATIVE_HANDLE_LIMIT ||
        !valid_diagnostic_bool(attempt->made_progress) ||
        !valid_diagnostic_bool(attempt->retryable) ||
        !valid_diagnostic_bool(attempt->close_failed) ||
        !valid_diagnostic_bool(attempt->blocked) ||
        !valid_diagnostic_bool(attempt->retired)) {
        return false;
    }
    if ((uint32_t)attempt->closed_resources +
            (uint32_t)attempt->consumed_error_resources +
            (uint32_t)attempt->retained_resources +
            (uint32_t)attempt->duplicate_references +
            (uint32_t)attempt->stale_entries +
            (uint32_t)attempt->invalid_entries !=
            attempt->handles_attempted ||
        attempt->callbacks_attempted > attempt->handles_attempted -
            attempt->duplicate_references ||
        attempt->retired_handles !=
            (uint32_t)attempt->closed_resources +
            (uint32_t)attempt->consumed_error_resources +
            (uint32_t)attempt->duplicate_references ||
        attempt->active_handles_after > attempt->active_handles_before ||
        attempt->active_handles_before - attempt->active_handles_after !=
            attempt->retired_handles ||
        attempt->active_objects_after > attempt->active_objects_before ||
        (uint32_t)attempt->active_objects_before -
            (uint32_t)attempt->active_objects_after !=
            (uint32_t)attempt->closed_resources +
            (uint32_t)attempt->consumed_error_resources ||
        attempt->made_progress != (attempt->retired_handles != 0U) ||
        attempt->retryable != (attempt->retained_resources != 0U) ||
        attempt->blocked != (attempt->active_handles_after != 0U) ||
        attempt->retired == attempt->blocked ||
        (!attempt->close_failed &&
            (attempt->consumed_error_resources != 0U ||
             attempt->retained_resources != 0U ||
             attempt->stale_entries != 0U ||
             attempt->invalid_entries != 0U))) {
        return false;
    }
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        const struct native_handle_close_type_summary *summary =
            &attempt->types[type];

        if (!summary_validate(summary)) {
            return false;
        }
        if (type == 0U && !summary_is_zero(summary)) {
            return false;
        }
        typed_attempts += summary->attempted_handles;
        typed_callbacks += summary->callback_attempts;
        typed_closed += summary->closed_resources;
        typed_consumed += summary->consumed_error_resources;
        typed_retained += summary->retained_resources;
        typed_duplicates += summary->duplicate_references;
        typed_stale += summary->stale_entries;
        typed_invalid += summary->invalid_entries;
        typed_retired += summary->retired_handles;
    }
    return typed_attempts <= attempt->handles_attempted &&
        typed_callbacks == attempt->callbacks_attempted &&
        typed_closed == attempt->closed_resources &&
        typed_consumed == attempt->consumed_error_resources &&
        typed_retained == attempt->retained_resources &&
        typed_duplicates == attempt->duplicate_references &&
        typed_stale == attempt->stale_entries &&
        typed_invalid <= attempt->invalid_entries &&
        typed_retired == attempt->retired_handles;
}

static bool attempt_is_zero(
    const struct native_process_teardown_attempt *attempt
)
{
    if (attempt == NULL || attempt->attempt_number != 0U ||
        attempt->handles_attempted != 0U ||
        attempt->callbacks_attempted != 0U || attempt->closed_resources != 0U ||
        attempt->consumed_error_resources != 0U ||
        attempt->retained_resources != 0U ||
        attempt->duplicate_references != 0U || attempt->stale_entries != 0U ||
        attempt->invalid_entries != 0U || attempt->retired_handles != 0U ||
        attempt->active_handles_before != 0U ||
        attempt->active_handles_after != 0U ||
        attempt->active_objects_before != 0U ||
        attempt->active_objects_after != 0U || attempt->made_progress ||
        attempt->retryable || attempt->close_failed || attempt->blocked ||
        attempt->retired) {
        return false;
    }
    for (size_t type = 0U; type < NATIVE_HANDLE_CLOSE_TYPE_COUNT; ++type) {
        if (!summary_is_zero(&attempt->types[type])) {
            return false;
        }
    }
    return true;
}

bool native_process_teardown_history_validate(
    const struct native_process_teardown_history *history
)
{
    if (history == NULL ||
        history->valid_entries > NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY ||
        history->insertion_cursor >= NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY ||
        !valid_diagnostic_bool(history->truncated)) {
        return false;
    }
    if (history->valid_entries < NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY) {
        if (history->insertion_cursor != history->valid_entries ||
            history->total_attempts != history->valid_entries ||
            history->dropped_attempts != 0U || history->truncated) {
            return false;
        }
    } else if (history->total_attempts <
            NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY) {
        return false;
    } else if (history->total_attempts != UINT64_MAX &&
            history->dropped_attempts != history->total_attempts -
                NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY) {
        return false;
    } else if (history->total_attempts == UINT64_MAX &&
            history->dropped_attempts < UINT64_MAX -
                NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY) {
        return false;
    }
    if (history->truncated != (history->dropped_attempts != 0U)) {
        return false;
    }
    for (size_t index = 0U; index < NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;
         ++index) {
        if (index < history->valid_entries) {
            if (!attempt_validate(&history->entries[index])) {
                return false;
            }
        } else if (!attempt_is_zero(&history->entries[index])) {
            return false;
        }
    }
    return true;
}

static void writer_init(
    struct diagnostic_writer *writer,
    char *output,
    size_t capacity
)
{
    writer->output = output;
    writer->capacity = capacity;
    writer->stored = 0U;
    writer->required = 0U;
    if (output != NULL && capacity != 0U) {
        output[0] = '\0';
    }
}

static void writer_byte(struct diagnostic_writer *writer, char value)
{
    if (writer->required != SIZE_MAX) {
        ++writer->required;
    }
    if (writer->output != NULL && writer->capacity > 1U &&
        writer->stored < writer->capacity - 1U) {
        writer->output[writer->stored] = value;
        ++writer->stored;
    }
}

static void writer_bytes(
    struct diagnostic_writer *writer,
    const char *text,
    size_t length
)
{
    for (size_t index = 0U; index < length; ++index) {
        writer_byte(writer, text[index]);
    }
}

#define WRITER_LITERAL(writer, literal) \
    writer_bytes((writer), (literal), sizeof(literal) - 1U)

static void writer_u64(struct diagnostic_writer *writer, uint64_t value)
{
    static const uint64_t divisors[20] = {
        UINT64_C(10000000000000000000), UINT64_C(1000000000000000000),
        UINT64_C(100000000000000000), UINT64_C(10000000000000000),
        UINT64_C(1000000000000000), UINT64_C(100000000000000),
        UINT64_C(10000000000000), UINT64_C(1000000000000),
        UINT64_C(100000000000), UINT64_C(10000000000),
        UINT64_C(1000000000), UINT64_C(100000000),
        UINT64_C(10000000), UINT64_C(1000000), UINT64_C(100000),
        UINT64_C(10000), UINT64_C(1000), UINT64_C(100), UINT64_C(10),
        UINT64_C(1)
    };
    bool started = false;

    for (size_t index = 0U; index < sizeof(divisors) / sizeof(divisors[0]);
         ++index) {
        const uint8_t digit = (uint8_t)(value / divisors[index] % 10U);

        if (digit != 0U || started || index ==
                sizeof(divisors) / sizeof(divisors[0]) - 1U) {
            writer_byte(writer, (char)('0' + digit));
            started = true;
        }
    }
}

static void writer_hex64(struct diagnostic_writer *writer, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";

    WRITER_LITERAL(writer, "0x");
    for (size_t index = 0U; index < 16U; ++index) {
        const size_t shift = (15U - index) * 4U;

        writer_byte(writer, digits[(value >> shift) & UINT64_C(0xF)]);
    }
}

static void writer_bool(struct diagnostic_writer *writer, bool value)
{
    writer_byte(writer, value ? '1' : '0');
}

static void writer_u16(struct diagnostic_writer *writer, uint16_t value)
{
    writer_u64(writer, value);
}

static void writer_status(
    struct diagnostic_writer *writer,
    enum native_handle_status status
)
{
    switch (status) {
    case NATIVE_HANDLE_OK:
        WRITER_LITERAL(writer, "ok");
        break;
    case NATIVE_HANDLE_NULL_ARGUMENT:
        WRITER_LITERAL(writer, "null_argument");
        break;
    case NATIVE_HANDLE_BAD_LIMIT:
        WRITER_LITERAL(writer, "bad_limit");
        break;
    case NATIVE_HANDLE_BAD_TYPE:
        WRITER_LITERAL(writer, "bad_type");
        break;
    case NATIVE_HANDLE_FULL:
        WRITER_LITERAL(writer, "full");
        break;
    case NATIVE_HANDLE_STALE:
        WRITER_LITERAL(writer, "stale");
        break;
    case NATIVE_HANDLE_WRONG_TYPE:
        WRITER_LITERAL(writer, "wrong_type");
        break;
    case NATIVE_HANDLE_CLOSE_FAILED:
        WRITER_LITERAL(writer, "close_failed");
        break;
    default:
        WRITER_LITERAL(writer, "unknown");
        break;
    }
}

static void writer_type(
    struct diagnostic_writer *writer,
    uint8_t type
)
{
    switch (type) {
    case PHIPIA_HANDLE_FILE:
        WRITER_LITERAL(writer, "file");
        break;
    case PHIPIA_HANDLE_DIRECTORY:
        WRITER_LITERAL(writer, "directory");
        break;
    case PHIPIA_HANDLE_WINDOW:
        WRITER_LITERAL(writer, "window");
        break;
    case PHIPIA_HANDLE_EVENT_QUEUE:
        WRITER_LITERAL(writer, "event_queue");
        break;
    case PHIPIA_HANDLE_STREAM:
        WRITER_LITERAL(writer, "stream");
        break;
    case PHIPIA_HANDLE_DATAGRAM:
        WRITER_LITERAL(writer, "datagram");
        break;
    case PHIPIA_HANDLE_TIMER:
        WRITER_LITERAL(writer, "timer");
        break;
    case PHIPIA_HANDLE_THREAD:
        WRITER_LITERAL(writer, "thread");
        break;
    case PHIPIA_HANDLE_AUDIO_OUTPUT:
        WRITER_LITERAL(writer, "audio_output");
        break;
    case PHIPIA_HANDLE_PACKAGE_UPLOAD:
        WRITER_LITERAL(writer, "package_upload");
        break;
    case PHIPIA_HANDLE_PACKAGE_CONTROL:
        WRITER_LITERAL(writer, "package_control");
        break;
    default:
        WRITER_LITERAL(writer, "unknown(");
        writer_u16(writer, type);
        writer_byte(writer, ')');
        break;
    }
}

static void writer_outcome(
    struct diagnostic_writer *writer,
    uint8_t outcome
)
{
    switch ((enum native_handle_close_outcome)outcome) {
    case NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED:
        WRITER_LITERAL(writer, "closed");
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_CONSUMED_ERROR:
        WRITER_LITERAL(writer, "consumed_error");
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED:
        WRITER_LITERAL(writer, "retained");
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE:
        WRITER_LITERAL(writer, "duplicate");
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_STALE:
        WRITER_LITERAL(writer, "stale");
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_INVALID:
        WRITER_LITERAL(writer, "invalid");
        break;
    default:
        WRITER_LITERAL(writer, "unknown");
        break;
    }
}

static void writer_summary(
    struct diagnostic_writer *writer,
    const struct native_handle_close_type_summary *summary
)
{
    WRITER_LITERAL(writer, "attempted=");
    writer_u16(writer, summary->attempted_handles);
    WRITER_LITERAL(writer, ",callbacks=");
    writer_u16(writer, summary->callback_attempts);
    WRITER_LITERAL(writer, ",closed=");
    writer_u16(writer, summary->closed_resources);
    WRITER_LITERAL(writer, ",consumed_error=");
    writer_u16(writer, summary->consumed_error_resources);
    WRITER_LITERAL(writer, ",retained=");
    writer_u16(writer, summary->retained_resources);
    WRITER_LITERAL(writer, ",duplicate=");
    writer_u16(writer, summary->duplicate_references);
    WRITER_LITERAL(writer, ",stale=");
    writer_u16(writer, summary->stale_entries);
    WRITER_LITERAL(writer, ",invalid=");
    writer_u16(writer, summary->invalid_entries);
    WRITER_LITERAL(writer, ",retired=");
    writer_u16(writer, summary->retired_handles);
}

static void writer_classes(
    struct diagnostic_writer *writer,
    const struct native_handle_close_type_summary *summaries
)
{
    WRITER_LITERAL(writer, "classes=[");
    for (size_t type = PHIPIA_HANDLE_FILE;
         type <= PHIPIA_HANDLE_PACKAGE_CONTROL; ++type) {
        if (type != PHIPIA_HANDLE_FILE) {
            writer_byte(writer, ';');
        }
        writer_type(writer, (uint8_t)type);
        writer_byte(writer, '{');
        writer_summary(writer, &summaries[type]);
        writer_byte(writer, '}');
    }
    writer_byte(writer, ']');
}

static void writer_report_entry(
    struct diagnostic_writer *writer,
    const struct native_handle_close_report_entry *entry
)
{
    WRITER_LITERAL(writer, "entry{handle=");
    writer_hex64(writer, entry->handle);
    WRITER_LITERAL(writer, ",object=");
    writer_u16(writer, entry->object_index);
    WRITER_LITERAL(writer, ",references=");
    writer_u16(writer, entry->references);
    WRITER_LITERAL(writer, ",type=");
    writer_type(writer, entry->type);
    WRITER_LITERAL(writer, ",outcome=");
    writer_outcome(writer, entry->outcome);
    writer_byte(writer, '}');
}

static void writer_report(
    struct diagnostic_writer *writer,
    const struct native_handle_close_report *report
)
{
    const size_t entry_count = report->attempted_handles <
        NATIVE_HANDLE_CLOSE_REPORT_CAPACITY ? report->attempted_handles :
        NATIVE_HANDLE_CLOSE_REPORT_CAPACITY;

    WRITER_LITERAL(writer, "report{status=");
    writer_status(writer, report->status);
    WRITER_LITERAL(writer, ",attempted=");
    writer_u16(writer, report->attempted_handles);
    WRITER_LITERAL(writer, ",callbacks=");
    writer_u16(writer, report->callback_attempts);
    WRITER_LITERAL(writer, ",closed=");
    writer_u16(writer, report->closed_resources);
    WRITER_LITERAL(writer, ",consumed_error=");
    writer_u16(writer, report->consumed_error_resources);
    WRITER_LITERAL(writer, ",retained=");
    writer_u16(writer, report->retained_resources);
    WRITER_LITERAL(writer, ",duplicate=");
    writer_u16(writer, report->duplicate_references);
    WRITER_LITERAL(writer, ",stale=");
    writer_u16(writer, report->stale_entries);
    WRITER_LITERAL(writer, ",invalid=");
    writer_u16(writer, report->invalid_entries);
    WRITER_LITERAL(writer, ",invalid_arguments=");
    writer_u16(writer, report->invalid_arguments);
    WRITER_LITERAL(writer, ",retired=");
    writer_u16(writer, report->retired_handles);
    WRITER_LITERAL(writer, ",omitted_entries=");
    writer_u16(writer, report->omitted_entries);
    WRITER_LITERAL(writer, ",before_handles=");
    writer_u16(writer, report->active_handles_before);
    WRITER_LITERAL(writer, ",after_handles=");
    writer_u16(writer, report->active_handles_after);
    WRITER_LITERAL(writer, ",before_objects=");
    writer_u16(writer, report->active_objects_before);
    WRITER_LITERAL(writer, ",after_objects=");
    writer_u16(writer, report->active_objects_after);
    WRITER_LITERAL(writer, ",retryable=");
    writer_bool(writer, report->retryable);
    WRITER_LITERAL(writer, ",progress=");
    writer_bool(writer, report->progress);
    WRITER_LITERAL(writer, ",truncated=");
    writer_bool(writer, report->truncated);
    writer_byte(writer, ',');
    writer_classes(writer, report->types);
    WRITER_LITERAL(writer, ",entries=[");
    for (size_t index = 0U; index < entry_count; ++index) {
        if (index != 0U) {
            writer_byte(writer, ';');
        }
        writer_report_entry(writer, &report->entries[index]);
    }
    WRITER_LITERAL(writer, "]}");
}

static void writer_attempt(
    struct diagnostic_writer *writer,
    const struct native_process_teardown_attempt *attempt
)
{
    WRITER_LITERAL(writer, "attempt{number=");
    writer_u64(writer, attempt->attempt_number);
    WRITER_LITERAL(writer, ",handles_attempted=");
    writer_u16(writer, attempt->handles_attempted);
    WRITER_LITERAL(writer, ",callbacks_attempted=");
    writer_u16(writer, attempt->callbacks_attempted);
    WRITER_LITERAL(writer, ",closed=");
    writer_u16(writer, attempt->closed_resources);
    WRITER_LITERAL(writer, ",consumed_error=");
    writer_u16(writer, attempt->consumed_error_resources);
    WRITER_LITERAL(writer, ",retained=");
    writer_u16(writer, attempt->retained_resources);
    WRITER_LITERAL(writer, ",duplicate=");
    writer_u16(writer, attempt->duplicate_references);
    WRITER_LITERAL(writer, ",stale=");
    writer_u16(writer, attempt->stale_entries);
    WRITER_LITERAL(writer, ",invalid=");
    writer_u16(writer, attempt->invalid_entries);
    WRITER_LITERAL(writer, ",retired_handles=");
    writer_u16(writer, attempt->retired_handles);
    WRITER_LITERAL(writer, ",before_handles=");
    writer_u16(writer, attempt->active_handles_before);
    WRITER_LITERAL(writer, ",after_handles=");
    writer_u16(writer, attempt->active_handles_after);
    WRITER_LITERAL(writer, ",before_objects=");
    writer_u16(writer, attempt->active_objects_before);
    WRITER_LITERAL(writer, ",after_objects=");
    writer_u16(writer, attempt->active_objects_after);
    WRITER_LITERAL(writer, ",progress=");
    writer_bool(writer, attempt->made_progress);
    WRITER_LITERAL(writer, ",retryable=");
    writer_bool(writer, attempt->retryable);
    WRITER_LITERAL(writer, ",blocked=");
    writer_bool(writer, attempt->blocked);
    WRITER_LITERAL(writer, ",retired=");
    writer_bool(writer, attempt->retired);
    WRITER_LITERAL(writer, ",close_failed=");
    writer_bool(writer, attempt->close_failed);
    writer_byte(writer, ',');
    writer_classes(writer, attempt->types);
    writer_byte(writer, '}');
}

static void writer_history(
    struct diagnostic_writer *writer,
    const struct native_process_teardown_history *history
)
{
    const size_t oldest = history->valid_entries ==
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY ?
        history->insertion_cursor : 0U;

    WRITER_LITERAL(writer, "history{total_attempts=");
    writer_u64(writer, history->total_attempts);
    WRITER_LITERAL(writer, ",valid_entries=");
    writer_u16(writer, history->valid_entries);
    WRITER_LITERAL(writer, ",dropped_attempts=");
    writer_u64(writer, history->dropped_attempts);
    WRITER_LITERAL(writer, ",truncated=");
    writer_bool(writer, history->truncated);
    WRITER_LITERAL(writer, ",entries=[");
    for (size_t offset = 0U; offset < history->valid_entries; ++offset) {
        const size_t index = (oldest + offset) %
            NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;

        if (offset != 0U) {
            writer_byte(writer, ';');
        }
        writer_attempt(writer, &history->entries[index]);
    }
    WRITER_LITERAL(writer, "]}");
}

static enum native_teardown_diagnostics_status writer_finish(
    struct diagnostic_writer *writer,
    size_t *required_length
)
{
    if (writer->output != NULL && writer->capacity != 0U) {
        writer->output[writer->stored] = '\0';
    }
    *required_length = writer->required;
    return writer->output == NULL || writer->capacity == 0U ||
            writer->required < writer->capacity ?
        NATIVE_TEARDOWN_DIAGNOSTICS_OK :
        NATIVE_TEARDOWN_DIAGNOSTICS_INSUFFICIENT_CAPACITY;
}

enum native_teardown_diagnostics_status native_handle_close_report_format(
    const struct native_handle_close_report *report,
    char *output,
    size_t output_capacity,
    size_t *required_length
)
{
    struct diagnostic_writer writer;

    writer_init(&writer, output, output_capacity);
    if (required_length == NULL || report == NULL ||
        (output == NULL && output_capacity != 0U)) {
        if (required_length != NULL) {
            *required_length = 0U;
        }
        return NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT;
    }
    if (!native_handle_close_report_validate(report)) {
        *required_length = 0U;
        return NATIVE_TEARDOWN_DIAGNOSTICS_INVALID;
    }
    writer_report(&writer, report);
    return writer_finish(&writer, required_length);
}

enum native_teardown_diagnostics_status
native_process_teardown_history_format(
    const struct native_process_teardown_history *history,
    char *output,
    size_t output_capacity,
    size_t *required_length
)
{
    struct diagnostic_writer writer;

    writer_init(&writer, output, output_capacity);
    if (required_length == NULL || history == NULL ||
        (output == NULL && output_capacity != 0U)) {
        if (required_length != NULL) {
            *required_length = 0U;
        }
        return NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT;
    }
    if (!native_process_teardown_history_validate(history)) {
        *required_length = 0U;
        return NATIVE_TEARDOWN_DIAGNOSTICS_INVALID;
    }
    writer_history(&writer, history);
    return writer_finish(&writer, required_length);
}

static uint32_t report_class_mask(
    const struct native_handle_close_report *report,
    uint8_t field
)
{
    uint32_t mask = 0U;

    if (report == NULL || !native_handle_close_report_validate(report)) {
        return 0U;
    }
    for (uint8_t type = PHIPIA_HANDLE_FILE;
         type <= PHIPIA_HANDLE_PACKAGE_CONTROL; ++type) {
        const struct native_handle_close_type_summary *summary =
            &report->types[type];
        const uint16_t value = field == 0U ? summary->retained_resources :
            field == 1U ? summary->consumed_error_resources :
            summary->retired_handles;

        if (value != 0U) {
            mask |= NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(type);
        }
    }
    return mask;
}

uint32_t native_handle_close_report_retained_class_mask(
    const struct native_handle_close_report *report
)
{
    return report_class_mask(report, 0U);
}

uint32_t native_handle_close_report_consumed_error_class_mask(
    const struct native_handle_close_report *report
)
{
    return report_class_mask(report, 1U);
}

uint32_t native_handle_close_report_retired_class_mask(
    const struct native_handle_close_report *report
)
{
    return report_class_mask(report, 2U);
}

static uint32_t history_class_mask(
    const struct native_process_teardown_history *history,
    uint8_t field,
    bool blocking_only
)
{
    uint32_t mask = 0U;
    size_t oldest;

    if (history == NULL || !native_process_teardown_history_validate(history)) {
        return 0U;
    }
    oldest = history->valid_entries == NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY ?
        history->insertion_cursor : 0U;
    for (size_t offset = 0U; offset < history->valid_entries; ++offset) {
        const size_t index = (oldest + offset) %
            NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;
        const struct native_process_teardown_attempt *attempt =
            &history->entries[index];

        if (blocking_only && !attempt->blocked) {
            continue;
        }
        for (uint8_t type = PHIPIA_HANDLE_FILE;
             type <= PHIPIA_HANDLE_PACKAGE_CONTROL; ++type) {
            const struct native_handle_close_type_summary *summary =
                &attempt->types[type];
            const uint16_t value = field == 0U ? summary->retained_resources :
                field == 1U ? summary->consumed_error_resources :
                summary->retired_handles;

            if (value != 0U) {
                mask |= NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(type);
            }
        }
    }
    return mask;
}

uint32_t native_process_teardown_history_retained_class_mask(
    const struct native_process_teardown_history *history
)
{
    return history_class_mask(history, 0U, false);
}

uint32_t native_process_teardown_history_blocking_class_mask(
    const struct native_process_teardown_history *history
)
{
    return history_class_mask(history, 0U, true);
}

uint32_t native_process_teardown_history_consumed_error_class_mask(
    const struct native_process_teardown_history *history
)
{
    return history_class_mask(history, 1U, false);
}

uint32_t native_process_teardown_history_retired_class_mask(
    const struct native_process_teardown_history *history
)
{
    return history_class_mask(history, 2U, false);
}

static bool history_copy_attempt_at(
    const struct native_process_teardown_history *history,
    size_t offset,
    struct native_process_teardown_attempt *output
)
{
    const size_t oldest = history->valid_entries ==
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY ?
        history->insertion_cursor : 0U;
    const size_t index = (oldest + offset) %
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;

    *output = history->entries[index];
    return true;
}

bool native_process_teardown_history_oldest_attempt(
    const struct native_process_teardown_history *history,
    struct native_process_teardown_attempt *output
)
{
    if (output == NULL || history == NULL ||
        !native_process_teardown_history_validate(history) ||
        history->valid_entries == 0U) {
        return false;
    }
    return history_copy_attempt_at(history, 0U, output);
}

bool native_process_teardown_history_latest_attempt(
    const struct native_process_teardown_history *history,
    struct native_process_teardown_attempt *output
)
{
    if (output == NULL || history == NULL ||
        !native_process_teardown_history_validate(history) ||
        history->valid_entries == 0U) {
        return false;
    }
    return history_copy_attempt_at(history, history->valid_entries - 1U,
        output);
}

bool native_process_teardown_history_newest_attempt(
    const struct native_process_teardown_history *history,
    struct native_process_teardown_attempt *output
)
{
    return native_process_teardown_history_latest_attempt(history, output);
}
