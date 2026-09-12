/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NATIVE_PROCESS_H
#define PHIPIA_NATIVE_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_PROCESS_LIMIT 4U
#define NATIVE_THREAD_LIMIT 8U
#define NATIVE_PROCESS_PAGE_LIMIT 4096U
#define NATIVE_STACK_PAGES 16U

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
};

struct interrupt_frame;
struct native_syscall_frame;

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
