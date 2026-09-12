/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_LINUX_SYSCALL_H
#define PHIPIA_LINUX_SYSCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/paging.h>

#define LINUX_SYSCALL_ALLOWLIST_COUNT 7U
#define LINUX_SYSCALL_ALLOWLIST_MAX 16U
#define LINUX_SYSCALL_EXPECTED_CALLS 9U
#define LINUX_SYSCALL_STDOUT_BYTES 7U
#define LINUX_SYSCALL_CPU_FOUNDATION_CONTROLS 10U
#define LINUX_SYSCALL_SEMANTIC_CONTROLS 11U
#define LINUX_UNAME_SYSCALL_ALLOWLIST_COUNT 6U
#define LINUX_UNAME_SYSCALL_EXPECTED_CALLS 6U
#define LINUX_UNAME_SYSCALL_STDOUT_BYTES 6U
#define LINUX_UNAME_SYSCALL_SEMANTIC_CONTROLS 18U
#define LINUX_UNAME_COPYOUT_CONTROLS 18U
#define LINUX_CAT_SYSCALL_ALLOWLIST_COUNT 5U
#define LINUX_CAT_SYSCALL_MIN_CALLS 4U
#define LINUX_CAT_SYSCALL_MAX_CALLS 12U
#define LINUX_CAT_READ_BUFFER UINT64_C(0x0000400001203F00)
#define LINUX_CAT_READ_COUNT 4096U
#define LINUX_CAT_INPUT_LINE_BYTES 64U
#define LINUX_CAT_INPUT_LINES 4U
#define LINUX_CAT_INPUT_TOTAL_BYTES 256U
#define LINUX_CAT_SYSCALL_SEMANTIC_CONTROLS 24U
#define LINUX_CAT_READ_NEGATIVE_CONTROLS 15U
#define LINUX_CAT_RESUME_NEGATIVE_CONTROLS 10U
#define LINUX_UTS_FIELD_BYTES 65U
#define LINUX_UTS_FIELD_COUNT 6U
#define LINUX_UTS_BYTES (LINUX_UTS_FIELD_BYTES * LINUX_UTS_FIELD_COUNT)

enum linux_syscall_profile {
    LINUX_SYSCALL_PROFILE_ECHO = 0,
    LINUX_SYSCALL_PROFILE_UNAME,
    LINUX_SYSCALL_PROFILE_CAT,
    LINUX_SYSCALL_PROFILE_COUNT
};

enum linux_cat_read_state {
    LINUX_CAT_READ_IDLE = 0,
    LINUX_CAT_READ_WAITING,
    LINUX_CAT_READ_READY,
    LINUX_CAT_READ_RESUMED,
    LINUX_CAT_READ_RELEASED,
    LINUX_CAT_READ_STATE_COUNT
};

enum linux_uts_copy_state {
    LINUX_UTS_COPY_CANDIDATE = 0,
    LINUX_UTS_COPY_ACTIVE,
    LINUX_UTS_COPY_COMPLETED,
    LINUX_UTS_COPY_FAILED,
    LINUX_UTS_COPY_RELEASED,
    LINUX_UTS_COPY_STATE_COUNT
};

enum linux_uname_stdout_state {
    LINUX_UNAME_STDOUT_CANDIDATE = 0,
    LINUX_UNAME_STDOUT_EMPTY,
    LINUX_UNAME_STDOUT_RECEIVING,
    LINUX_UNAME_STDOUT_VALID,
    LINUX_UNAME_STDOUT_INVALID,
    LINUX_UNAME_STDOUT_RELEASED,
    LINUX_UNAME_STDOUT_STATE_COUNT
};

enum linux_syscall_cpu_state {
    LINUX_SYSCALL_CPU_CANDIDATE = 0,
    LINUX_SYSCALL_CPU_ARMED,
    LINUX_SYSCALL_CPU_ENTERED,
    LINUX_SYSCALL_CPU_RETURNED,
    LINUX_SYSCALL_CPU_DISARMED,
    LINUX_SYSCALL_CPU_STATE_COUNT
};

enum linux_syscall_status {
    LINUX_SYSCALL_STATUS_OK = 0,
    LINUX_SYSCALL_STATUS_NULL_ARGUMENT,
    LINUX_SYSCALL_STATUS_BUSY,
    LINUX_SYSCALL_STATUS_UNSUPPORTED_CPU,
    LINUX_SYSCALL_STATUS_DESCRIPTOR_CONTRACT,
    LINUX_SYSCALL_STATUS_MSR_CONTRACT,
    LINUX_SYSCALL_STATUS_BAD_STATE,
    LINUX_SYSCALL_STATUS_BAD_PROCESS,
    LINUX_SYSCALL_STATUS_BAD_GENERATION,
    LINUX_SYSCALL_STATUS_BAD_CR3,
    LINUX_SYSCALL_STATUS_BAD_STACK,
    LINUX_SYSCALL_STATUS_BAD_ENTRY,
    LINUX_SYSCALL_STATUS_BAD_RETURN,
    LINUX_SYSCALL_STATUS_BAD_PROVENANCE,
    LINUX_SYSCALL_STATUS_BAD_ORDER,
    LINUX_SYSCALL_STATUS_BAD_ARGUMENT,
    LINUX_SYSCALL_STATUS_BAD_POINTER,
    LINUX_SYSCALL_STATUS_MAPPING,
    LINUX_SYSCALL_STATUS_COPYOUT,
    LINUX_SYSCALL_STATUS_STDOUT,
    LINUX_SYSCALL_STATUS_EXIT,
    LINUX_SYSCALL_STATUS_CONTROLLED_FAILURE,
    LINUX_SYSCALL_STATUS_RESTORE,
    LINUX_SYSCALL_STATUS_COUNT
};

/* Exact stack image built by src/arch/x86_64/linux_syscall.S. */
struct linux_syscall_frame {
    uint64_t rax;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

struct linux_syscall_context {
    enum linux_syscall_profile profile;
    struct paging_process_space *address_space;
    uint64_t process_generation;
    uint64_t executable_start;
    uint64_t executable_end;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t fs_address;
    uint64_t tid_address;
    uintptr_t heap_frames[PAGING_LINUX_HEAP_PAGES];
    uintptr_t anonymous_frame;
    uintptr_t stack_frames[PAGING_LINUX_STACK_PAGES];
    bool (*exit_observed)(uint64_t process_generation);
    uint32_t failure_before_ordinal;
    uint32_t failure_after_ordinal;
    bool controlled_run;
    bool publish_stdout;
};

struct linux_syscall_result {
    uint32_t syscall_count;
    uint32_t distinct_syscalls;
    uint32_t stdout_bytes;
    uint32_t exit_status;
    enum linux_syscall_status status;
    enum linux_syscall_cpu_state cpu_state;
    bool stdout_valid;
    bool exit_zero;
    bool real_syscall_instruction;
    bool process_authenticated;
    bool cr3_authenticated;
    bool cpu_disarmed;
    bool controlled_failure_observed;
    bool uts_copy_valid;
    uint32_t cat_input_bytes;
    uint32_t cat_input_lines;
    uint32_t cat_resume_count;
    bool cat_wait_observed;
    bool cat_eof_delivered;
};

bool linux_syscall_cpu_foundation_self_test(size_t *completed_tests);
bool linux_syscall_enosys_self_test(void);
bool linux_syscall_semantic_self_test(void);
bool linux_syscall_uname_semantic_self_test(void);
bool linux_syscall_uname_copyout_self_test(size_t *completed_tests);
bool linux_syscall_cat_semantic_self_test(void);
bool linux_syscall_cat_read_negative_self_test(size_t *completed_tests);
bool linux_syscall_cat_resume_negative_self_test(
    uint64_t process_generation,
    size_t *completed_tests
);
enum linux_syscall_status linux_syscall_arm(
    const struct linux_syscall_context *context
);
enum linux_syscall_status linux_syscall_validate_armed(void);
enum linux_syscall_status linux_syscall_disarm(void);
struct linux_syscall_result linux_syscall_get_result(void);
bool linux_syscall_resources_released(void);
bool linux_syscall_cat_waiting(uint64_t process_generation);
enum linux_syscall_status linux_syscall_cat_complete_read(
    uint64_t process_generation,
    const uint8_t *bytes,
    size_t byte_count,
    bool eof
);
const struct linux_syscall_frame *linux_syscall_cat_resume_frame(
    uint64_t process_generation
);
uintptr_t linux_syscall_dispatch(struct linux_syscall_frame *frame);

void linux_process_enter_user(uint64_t entry, uint64_t stack_pointer);
void linux_process_resume_user(const struct linux_syscall_frame *frame);
uintptr_t linux_process_resume_stack(void);
bool linux_process_boundary_active(void);
extern const uint8_t linux_syscall_entry[];

#endif
