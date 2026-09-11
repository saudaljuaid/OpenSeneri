/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NATIVE_HANDLE_H
#define PHIPIA_NATIVE_HANDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/abi/base.h>

#define NATIVE_HANDLE_LIMIT 128U
#define NATIVE_RESOURCE_WORDS 4U
#define NATIVE_HANDLE_CLOSE_REPORT_CAPACITY 32U

enum native_handle_status {
    NATIVE_HANDLE_OK = 0,
    NATIVE_HANDLE_NULL_ARGUMENT,
    NATIVE_HANDLE_BAD_LIMIT,
    NATIVE_HANDLE_BAD_TYPE,
    NATIVE_HANDLE_FULL,
    NATIVE_HANDLE_STALE,
    NATIVE_HANDLE_WRONG_TYPE,
    NATIVE_HANDLE_CLOSE_FAILED,
    NATIVE_HANDLE_STATUS_COUNT
};

struct native_resource {
    uint64_t words[NATIVE_RESOURCE_WORDS];
};

struct native_handle_slot {
    uint32_t generation;
    uint16_t object_index;
    uint8_t type;
    bool active;
};

struct native_handle_object {
    struct native_resource resource;
    uint16_t references;
    uint8_t type;
    bool active;
};

struct native_handle_table {
    struct native_handle_slot slots[NATIVE_HANDLE_LIMIT];
    struct native_handle_object objects[NATIVE_HANDLE_LIMIT];
    uint16_t limit;
    uint16_t active_handles;
    uint16_t active_objects;
    bool initialized;
};

enum native_resource_close_result {
    NATIVE_RESOURCE_RETAINED = 0,
    NATIVE_RESOURCE_CLOSED = 1,
    NATIVE_RESOURCE_CLOSED_WITH_ERROR = 2
};

/* One bounded census entry describes a slot visited by close_all. */
enum native_handle_close_outcome {
    NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED = 0,
    NATIVE_HANDLE_CLOSE_OUTCOME_CONSUMED_ERROR,
    NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED,
    NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE,
    NATIVE_HANDLE_CLOSE_OUTCOME_STALE,
    NATIVE_HANDLE_CLOSE_OUTCOME_INVALID,
    NATIVE_HANDLE_CLOSE_OUTCOME_COUNT
};

struct native_handle_close_report_entry {
    phipia_handle_t handle;
    uint16_t object_index;
    uint16_t references;
    uint8_t type;
    uint8_t outcome;
};

/*
 * close_all reports exact counters even when the handle list is truncated.
 * The fixed list keeps teardown diagnostics allocation-free and stack-safe.
 */
struct native_handle_close_report {
    struct native_handle_close_report_entry entries[
        NATIVE_HANDLE_CLOSE_REPORT_CAPACITY];
    uint16_t attempted_handles;
    uint16_t callback_attempts;
    uint16_t closed_resources;
    uint16_t consumed_error_resources;
    uint16_t retained_resources;
    uint16_t stale_entries;
    uint16_t invalid_entries;
    uint16_t invalid_arguments;
    uint16_t duplicate_references;
    uint16_t retired_handles;
    uint16_t omitted_entries;
    uint16_t active_handles_before;
    uint16_t active_handles_after;
    uint16_t active_objects_before;
    uint16_t active_objects_after;
    enum native_handle_status status;
    bool retryable;
    bool progress;
    bool truncated;
};

/* A consumed resource must retire even when its final writeback failed. */
typedef enum native_resource_close_result (*native_handle_close_fn)(
    uint8_t type,
    const struct native_resource *resource,
    void *context
);

enum native_handle_status native_handle_table_initialize(
    struct native_handle_table *table,
    uint16_t limit
);
enum native_handle_status native_handle_install(
    struct native_handle_table *table,
    uint8_t type,
    const struct native_resource *resource,
    phipia_handle_t *handle
);
enum native_handle_status native_handle_resolve(
    struct native_handle_table *table,
    phipia_handle_t handle,
    uint8_t expected_type,
    struct native_resource **resource
);
enum native_handle_status native_handle_duplicate(
    struct native_handle_table *table,
    phipia_handle_t source,
    phipia_handle_t *duplicate
);
enum native_handle_status native_handle_close(
    struct native_handle_table *table,
    phipia_handle_t handle,
    native_handle_close_fn close_resource,
    void *context
);
enum native_handle_status native_handle_close_all(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context
);
/* Report whether close_all left a wrapper that can be retried by teardown. */
enum native_handle_status native_handle_close_all_report(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context,
    bool *retryable
);
void native_handle_close_report_reset(
    struct native_handle_close_report *report
);
enum native_handle_status native_handle_close_with_report(
    struct native_handle_table *table,
    phipia_handle_t handle,
    native_handle_close_fn close_resource,
    void *context,
    struct native_handle_close_report *report
);
enum native_handle_status native_handle_close_all_diagnostics(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context,
    struct native_handle_close_report *report
);
bool native_handle_self_test(size_t *completed_tests);

#endif
