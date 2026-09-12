/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_LINUX_UNAME_H
#define PHIPIA_LINUX_UNAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINUX_UNAME_ABI_IMAGE_BYTES 38368U
#define LINUX_UNAME_ABI_EXECUTABLE_START UINT64_C(0x0000400001001000)
#define LINUX_UNAME_ABI_EXECUTABLE_END UINT64_C(0x0000400001008000)
#define LINUX_UNAME_ABI_FS_ADDRESS UINT64_C(0x000040000100AC58)
#define LINUX_UNAME_ABI_TID_ADDRESS UINT64_C(0x000040000100AE0C)
#define LINUX_UNAME_ABI_STACK_FOUNDATION_CONTROLS 8U
#define LINUX_UNAME_ABI_IMAGE_UTS_FOUNDATION_CONTROLS 50U
#define LINUX_UNAME_ABI_LIFECYCLE_CONTROLS 7U
#define LINUX_UNAME_ABI_CONTROLLED_ROBUSTNESS_TESTS 97U

enum linux_uname_process_state {
    LINUX_UNAME_PROCESS_CANDIDATE = 0,
    LINUX_UNAME_PROCESS_BUILDING,
    LINUX_UNAME_PROCESS_INSTALLED,
    LINUX_UNAME_PROCESS_RUNNING,
    LINUX_UNAME_PROCESS_EXITING,
    LINUX_UNAME_PROCESS_STOPPING,
    LINUX_UNAME_PROCESS_RELEASED,
    LINUX_UNAME_PROCESS_STATE_COUNT
};

enum linux_uname_executable_state {
    LINUX_UNAME_EXECUTABLE_CANDIDATE = 0,
    LINUX_UNAME_EXECUTABLE_VALIDATED,
    LINUX_UNAME_EXECUTABLE_INSTALLED,
    LINUX_UNAME_EXECUTABLE_RELEASED,
    LINUX_UNAME_EXECUTABLE_STATE_COUNT
};

enum linux_uname_stack_state {
    LINUX_UNAME_STACK_CANDIDATE = 0,
    LINUX_UNAME_STACK_BUILDING,
    LINUX_UNAME_STACK_INSTALLED,
    LINUX_UNAME_STACK_RELEASED,
    LINUX_UNAME_STACK_STATE_COUNT
};

enum linux_uname_abi_status {
    LINUX_UNAME_ABI_STATUS_OK = 0,
    LINUX_UNAME_ABI_STATUS_ABSENT,
    LINUX_UNAME_ABI_STATUS_NULL_ARGUMENT,
    LINUX_UNAME_ABI_STATUS_BUSY,
    LINUX_UNAME_ABI_STATUS_PREREQUISITE,
    LINUX_UNAME_ABI_STATUS_FILESYSTEM,
    LINUX_UNAME_ABI_STATUS_ELF,
    LINUX_UNAME_ABI_STATUS_ELF_INSTALL,
    LINUX_UNAME_ABI_STATUS_STACK,
    LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION,
    LINUX_UNAME_ABI_STATUS_ADDRESS_SPACE,
    LINUX_UNAME_ABI_STATUS_ALIAS,
    LINUX_UNAME_ABI_STATUS_MAPPING,
    LINUX_UNAME_ABI_STATUS_PERMISSION_AUDIT,
    LINUX_UNAME_ABI_STATUS_SYSCALL_CPU,
    LINUX_UNAME_ABI_STATUS_ENTRY,
    LINUX_UNAME_ABI_STATUS_EXIT,
    LINUX_UNAME_ABI_STATUS_SYSCALL_CONTROL,
    LINUX_UNAME_ABI_STATUS_TRANSITION_REPEATED,
    LINUX_UNAME_ABI_STATUS_TRANSITION_REVERSED,
    LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID,
    LINUX_UNAME_ABI_STATUS_TEARDOWN,
    LINUX_UNAME_ABI_STATUS_RESOURCE_CENSUS,
    LINUX_UNAME_ABI_STATUS_ROBUSTNESS,
    LINUX_UNAME_ABI_STATUS_COUNT
};

struct linux_uname_abi_proof_result {
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
};

bool linux_uname_image_uts_foundation_self_test(size_t *completed_tests);
enum linux_uname_abi_status linux_uname_abi_installed_prove(
    struct linux_uname_abi_proof_result *result
);
enum linux_uname_abi_status linux_uname_abi_launch(
    struct linux_uname_abi_proof_result *result
);
struct linux_uname_abi_proof_result linux_uname_abi_get_proof_result(void);
bool linux_uname_abi_resources_released(void);
const char *linux_uname_abi_status_string(enum linux_uname_abi_status status);

#endif
