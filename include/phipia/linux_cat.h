/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_LINUX_CAT_H
#define PHIPIA_LINUX_CAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINUX_CAT_ABI_IMAGE_BYTES 38632U
#define LINUX_CAT_ABI_EXECUTABLE_START UINT64_C(0x0000400001001000)
#define LINUX_CAT_ABI_EXECUTABLE_END UINT64_C(0x0000400001008000)
#define LINUX_CAT_ABI_FS_ADDRESS UINT64_C(0x000040000100B178)
#define LINUX_CAT_ABI_TID_ADDRESS UINT64_C(0x000040000100B324)
#define LINUX_CAT_ABI_STACK_FOUNDATION_CONTROLS 7U
#define LINUX_CAT_ABI_IMAGE_STDIN_FOUNDATION_CONTROLS 55U
#define LINUX_CAT_ABI_LIFECYCLE_CONTROLS 10U
#define LINUX_CAT_ABI_RUNTIME_NEGATIVE_CONTROLS 28U

enum linux_cat_process_state {
    LINUX_CAT_PROCESS_CANDIDATE = 0,
    LINUX_CAT_PROCESS_BUILDING,
    LINUX_CAT_PROCESS_INSTALLED,
    LINUX_CAT_PROCESS_RUNNING,
    LINUX_CAT_PROCESS_WAITING_FOR_INPUT,
    LINUX_CAT_PROCESS_READY_TO_RESUME,
    LINUX_CAT_PROCESS_EXITING,
    LINUX_CAT_PROCESS_STOPPING,
    LINUX_CAT_PROCESS_RELEASED,
    LINUX_CAT_PROCESS_STATE_COUNT
};

enum linux_cat_executable_state {
    LINUX_CAT_EXECUTABLE_CANDIDATE = 0,
    LINUX_CAT_EXECUTABLE_VALIDATED,
    LINUX_CAT_EXECUTABLE_INSTALLED,
    LINUX_CAT_EXECUTABLE_RELEASED,
    LINUX_CAT_EXECUTABLE_STATE_COUNT
};

enum linux_cat_stack_state {
    LINUX_CAT_STACK_CANDIDATE = 0,
    LINUX_CAT_STACK_BUILDING,
    LINUX_CAT_STACK_INSTALLED,
    LINUX_CAT_STACK_RELEASED,
    LINUX_CAT_STACK_STATE_COUNT
};

enum linux_cat_abi_status {
    LINUX_CAT_ABI_STATUS_OK = 0,
    LINUX_CAT_ABI_STATUS_WAITING,
    LINUX_CAT_ABI_STATUS_ABSENT,
    LINUX_CAT_ABI_STATUS_NULL_ARGUMENT,
    LINUX_CAT_ABI_STATUS_BUSY,
    LINUX_CAT_ABI_STATUS_PREREQUISITE,
    LINUX_CAT_ABI_STATUS_FILESYSTEM,
    LINUX_CAT_ABI_STATUS_ELF,
    LINUX_CAT_ABI_STATUS_ELF_INSTALL,
    LINUX_CAT_ABI_STATUS_STACK,
    LINUX_CAT_ABI_STATUS_FRAME_ALLOCATION,
    LINUX_CAT_ABI_STATUS_ADDRESS_SPACE,
    LINUX_CAT_ABI_STATUS_ALIAS,
    LINUX_CAT_ABI_STATUS_MAPPING,
    LINUX_CAT_ABI_STATUS_PERMISSION_AUDIT,
    LINUX_CAT_ABI_STATUS_SYSCALL_CPU,
    LINUX_CAT_ABI_STATUS_ENTRY,
    LINUX_CAT_ABI_STATUS_EXIT,
    LINUX_CAT_ABI_STATUS_INPUT,
    LINUX_CAT_ABI_STATUS_SYSCALL_CONTROL,
    LINUX_CAT_ABI_STATUS_TRANSITION_REPEATED,
    LINUX_CAT_ABI_STATUS_TRANSITION_REVERSED,
    LINUX_CAT_ABI_STATUS_TRANSITION_INVALID,
    LINUX_CAT_ABI_STATUS_TEARDOWN,
    LINUX_CAT_ABI_STATUS_RESOURCE_CENSUS,
    LINUX_CAT_ABI_STATUS_ROBUSTNESS,
    LINUX_CAT_ABI_STATUS_COUNT
};

struct linux_cat_abi_proof_result {
    uint32_t file_bytes;
    uint32_t program_headers;
    uint32_t load_segments;
    uint32_t file_clusters;
    uint32_t stdout_bytes;
    uint32_t syscall_count;
    uint32_t distinct_syscalls;
    uint32_t exit_status;
    uint32_t robustness_tests;
    bool ring_three;
    bool private_address_space;
    bool real_syscall_instruction;
    bool uts_copy_valid;
    bool stdout_valid;
    bool exit_zero;
    bool unknown_enosys;
    bool write_xor_execute;
    bool kernel_cr3_restored;
    bool teardown_complete;
    bool resource_census_equal;
    uint32_t input_bytes;
    uint32_t input_lines;
    uint32_t resume_count;
    uint64_t generation;
    bool waiting_for_input;
    bool eof_delivered;
};

bool linux_cat_image_stdin_foundation_self_test(size_t *completed_tests);
enum linux_cat_abi_status linux_cat_abi_installed_prove(
    struct linux_cat_abi_proof_result *result
);
enum linux_cat_abi_status linux_cat_abi_launch(
    struct linux_cat_abi_proof_result *result
);
enum linux_cat_abi_status linux_cat_abi_deliver_input(
    const uint8_t *bytes,
    size_t byte_count,
    bool eof,
    struct linux_cat_abi_proof_result *result
);
bool linux_cat_abi_waiting(void);
uint64_t linux_cat_abi_generation(void);
enum linux_cat_abi_status linux_cat_abi_abort(void);
struct linux_cat_abi_proof_result linux_cat_abi_get_proof_result(void);
bool linux_cat_abi_resources_released(void);
const char *linux_cat_abi_status_string(enum linux_cat_abi_status status);

#endif
