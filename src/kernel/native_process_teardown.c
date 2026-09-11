/* SPDX-License-Identifier: GPL-3.0-only */
/* Fixed-size process teardown history storage. */

#include <phipia/native_process.h>

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(
    void *destination,
    const void *source,
    size_t length
)
{
    uint8_t *output = destination;
    const uint8_t *input = source;

    for (size_t index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
}

static bool history_shape_valid(
    const struct native_process_teardown_history *history
)
{
    return history != NULL &&
        history->valid_entries <= NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY &&
        history->insertion_cursor <
            NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;
}

void native_process_teardown_history_reset(
    struct native_process_teardown_history *history
)
{
    if (history != NULL) {
        zero_bytes(history, sizeof(*history));
    }
}

void native_process_teardown_history_append(
    struct native_process_teardown_history *history,
    const struct native_process_teardown_attempt *attempt
)
{
    uint16_t index;

    if (history == NULL || attempt == NULL) {
        return;
    }
    if (!history_shape_valid(history)) {
        native_process_teardown_history_reset(history);
    }
    index = history->insertion_cursor;
    history->entries[index] = *attempt;
    history->insertion_cursor = (uint16_t)((index + 1U) %
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY);
    if (history->valid_entries < NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY) {
        ++history->valid_entries;
    } else {
        if (history->dropped_attempts != UINT64_MAX) {
            ++history->dropped_attempts;
        }
        history->truncated = true;
    }
    if (history->total_attempts != UINT64_MAX) {
        ++history->total_attempts;
    }
}

size_t native_process_teardown_history_copy(
    const struct native_process_teardown_history *history,
    struct native_process_teardown_attempt *output,
    size_t output_capacity
)
{
    size_t count;
    size_t oldest;
    size_t start;

    if (output == NULL || output_capacity == 0U ||
        !history_shape_valid(history)) {
        return 0U;
    }
    count = history->valid_entries;
    if (count > output_capacity) {
        count = output_capacity;
    }
    oldest = history->valid_entries ==
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY ?
        history->insertion_cursor : 0U;
    start = history->valid_entries - count;
    for (size_t offset = 0U; offset < count; ++offset) {
        const size_t index = (oldest + start + offset) %
            NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY;

        copy_bytes(&output[offset], &history->entries[index],
            sizeof(output[offset]));
    }
    return count;
}
