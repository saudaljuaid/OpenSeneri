/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NATIVE_TEARDOWN_DIAGNOSTICS_H
#define PHIPIA_NATIVE_TEARDOWN_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/native_process.h>

/* One bit per known resource class; bit zero represents handle type one. */
#define NATIVE_TEARDOWN_RESOURCE_CLASS_MASK(type) \
    ((uint32_t)(type) >= PHIPIA_HANDLE_FILE && \
        (uint32_t)(type) <= PHIPIA_HANDLE_PACKAGE_CONTROL ? \
        (UINT32_C(1) << ((uint32_t)(type) - PHIPIA_HANDLE_FILE)) : 0U)

enum native_teardown_diagnostics_status {
    NATIVE_TEARDOWN_DIAGNOSTICS_OK = 0,
    NATIVE_TEARDOWN_DIAGNOSTICS_NULL_ARGUMENT,
    NATIVE_TEARDOWN_DIAGNOSTICS_INVALID,
    NATIVE_TEARDOWN_DIAGNOSTICS_INSUFFICIENT_CAPACITY,
    NATIVE_TEARDOWN_DIAGNOSTICS_STATUS_COUNT
};

/* Validators are read-only and bound every scan by the fixed capacities. */
bool native_handle_close_report_validate(
    const struct native_handle_close_report *report
);
bool native_process_teardown_history_validate(
    const struct native_process_teardown_history *history
);

/*
 * Formatters report the required byte count without the terminating NUL.
 * A NULL output is valid only with output_capacity == 0U (a size query).
 */
enum native_teardown_diagnostics_status native_handle_close_report_format(
    const struct native_handle_close_report *report,
    char *output,
    size_t output_capacity,
    size_t *required_length
);
enum native_teardown_diagnostics_status
native_process_teardown_history_format(
    const struct native_process_teardown_history *history,
    char *output,
    size_t output_capacity,
    size_t *required_length
);

/* Derived masks use only already-populated report/history summaries. */
uint32_t native_handle_close_report_retained_class_mask(
    const struct native_handle_close_report *report
);
uint32_t native_handle_close_report_consumed_error_class_mask(
    const struct native_handle_close_report *report
);
uint32_t native_handle_close_report_retired_class_mask(
    const struct native_handle_close_report *report
);
uint32_t native_process_teardown_history_retained_class_mask(
    const struct native_process_teardown_history *history
);
uint32_t native_process_teardown_history_blocking_class_mask(
    const struct native_process_teardown_history *history
);
uint32_t native_process_teardown_history_consumed_error_class_mask(
    const struct native_process_teardown_history *history
);
uint32_t native_process_teardown_history_retired_class_mask(
    const struct native_process_teardown_history *history
);

/* Copy-out accessors never return pointers into the ring buffer. */
bool native_process_teardown_history_latest_attempt(
    const struct native_process_teardown_history *history,
    struct native_process_teardown_attempt *output
);
bool native_process_teardown_history_oldest_attempt(
    const struct native_process_teardown_history *history,
    struct native_process_teardown_attempt *output
);
bool native_process_teardown_history_newest_attempt(
    const struct native_process_teardown_history *history,
    struct native_process_teardown_attempt *output
);

#endif
