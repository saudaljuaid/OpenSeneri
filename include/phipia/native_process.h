/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NATIVE_PROCESS_H
#define PHIPIA_NATIVE_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/native_handle.h>

#define NATIVE_PROCESS_LIMIT 4U
#define NATIVE_THREAD_LIMIT 8U
#define NATIVE_PROCESS_PAGE_LIMIT 4096U
#define NATIVE_STACK_PAGES 16U
#define NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY 8U

enum native_process_status {
    NATIVE_PROCESS_OK = 0,
    NATIVE_PROCESS_NULL_ARGUMENT,
    NATIVE_PROCESS_BUSY,
    NATIVE_PROCESS_NO_SLOT,
    NATIVE_PROCESS_MANIFEST_OPEN,
    NATIVE_PROCESS_MANIFEST_READ,
    NATIVE_PROCESS_EXECUTABLE_OPEN,
    NATIVE_PROCESS_EXECUTABLE_READ,
    NATIVE_PROCESS_IMAGE_REFUSED,
    NATIVE_PROCESS_DATA_NAMESPACE,
    NATIVE_PROCESS_MEMORY_LIMIT,
    NATIVE_PROCESS_FRAME_ALLOCATION,
    NATIVE_PROCESS_ADDRESS_SPACE,
    NATIVE_PROCESS_MAPPING,
    NATIVE_PROCESS_STACK,
    NATIVE_PROCESS_CPU,
    NATIVE_PROCESS_GATE,
    NATIVE_PROCESS_SYSCALL,
    NATIVE_PROCESS_TEARDOWN,
    NATIVE_PROCESS_STATUS_COUNT
};

enum native_process_failure_stage {
    NATIVE_PROCESS_FAILURE_NONE = 0,
    NATIVE_PROCESS_FAILURE_GATE_VALIDATE,
    NATIVE_PROCESS_FAILURE_GATE_REARM,
    NATIVE_PROCESS_FAILURE_ADDRESS_SPACE_ACTIVATE,
    NATIVE_PROCESS_FAILURE_FPU_RESTORE
};

struct native_process_teardown_attempt {
    uint64_t attempt_number;
    uint16_t handles_attempted;
    uint16_t callbacks_attempted;
    uint16_t closed_resources;
    uint16_t consumed_error_resources;
    uint16_t retained_resources;
    uint16_t duplicate_references;
    uint16_t stale_entries;
    uint16_t invalid_entries;
    uint16_t retired_handles;
    uint16_t active_handles_before;
    uint16_t active_handles_after;
    uint16_t active_objects_before;
    uint16_t active_objects_after;
    bool made_progress;
    bool retryable;
    bool close_failed;
    bool blocked;
    bool retired;
    struct native_handle_close_type_summary types[
        NATIVE_HANDLE_CLOSE_TYPE_COUNT];
};

struct native_process_teardown_history {
    struct native_process_teardown_attempt entries[
        NATIVE_PROCESS_TEARDOWN_HISTORY_CAPACITY];
    uint16_t valid_entries;
    uint16_t insertion_cursor;
    uint64_t total_attempts;
    uint64_t dropped_attempts;
    bool truncated;
};

struct native_process_teardown_report {
    struct native_handle_close_report handles;
    struct native_process_teardown_history history;
    uint32_t attempts;
    bool blocked;
    bool retired;
};

struct native_process_result {
    uint64_t generation;
    int32_t exit_status;
    uint32_t syscall_count;
    uint32_t last_syscall;
    uint32_t failure_stage;
    uint32_t thread_switches;
    uint32_t peak_pages;
    uint32_t peak_handles;
    uint64_t context_cycles_without_fpu;
    uint64_t context_cycles_with_fpu;
    uint32_t context_transition_samples;
    bool exited;
    bool faulted;
    bool resources_released;
    struct native_process_teardown_report teardown_report;
};

struct interrupt_frame;
struct native_syscall_frame;

/* NULL reset is a no-op; reset clears entries and truncation metadata. */
void native_process_teardown_history_reset(
    struct native_process_teardown_history *history
);
/* Full history overwrites its oldest entry and records truncation. */
void native_process_teardown_history_append(
    struct native_process_teardown_history *history,
    const struct native_process_teardown_attempt *attempt
);
/* Copies the newest entries oldest-to-newest; NULL or zero capacity copies none. */
size_t native_process_teardown_history_copy(
    const struct native_process_teardown_history *history,
    struct native_process_teardown_attempt *output,
    size_t output_capacity
);

enum native_process_status native_process_spawn(
    const char *manifest_path,
    uint64_t *generation
);
enum native_process_status native_process_run(struct native_process_result *result);
enum native_process_status native_process_launch(
    const char *manifest_path,
    struct native_process_result *result
);
enum native_process_status native_process_launch_installed(
    const char *manifest_path,
    struct native_process_result *result
);
bool native_process_resources_released(void);
bool native_process_self_test(size_t *completed_tests);
const char *native_process_status_string(enum native_process_status status);
uintptr_t native_process_on_syscall(struct native_syscall_frame *frame);
void native_process_on_interrupt(struct interrupt_frame *frame, void *context);
bool native_process_interrupt_active(void);

#endif
