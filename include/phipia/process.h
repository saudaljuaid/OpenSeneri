/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PROCESS_H
#define PHIPIA_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROCESS_ADDRESS_SPACE_FOUNDATION_CONTROLS 8U
#define PROCESS_CONTROLLED_ROBUSTNESS_TESTS 50U

enum process_state {
    PROCESS_CANDIDATE = 0,
    PROCESS_BUILDING,
    PROCESS_INSTALLED,
    PROCESS_RUNNING,
    PROCESS_STOPPING,
    PROCESS_RELEASED,
    PROCESS_STATE_COUNT
};

enum process_image_state {
    PROCESS_IMAGE_CANDIDATE = 0,
    PROCESS_IMAGE_VALIDATED,
    PROCESS_IMAGE_EXTENT_CHECKED,
    PROCESS_IMAGE_FRAME_ALLOCATED,
    PROCESS_IMAGE_INITIALIZED,
    PROCESS_IMAGE_MAPPED,
    PROCESS_IMAGE_EXECUTABLE,
    PROCESS_IMAGE_RECLAIMED,
    PROCESS_IMAGE_STATE_COUNT
};

enum process_stack_state {
    PROCESS_STACK_UNALLOCATED = 0,
    PROCESS_STACK_ALLOCATED,
    PROCESS_STACK_GUARDED,
    PROCESS_STACK_MAPPED,
    PROCESS_STACK_ACTIVE,
    PROCESS_STACK_RECLAIMED,
    PROCESS_STACK_STATE_COUNT
};

enum process_status {
    PROCESS_STATUS_OK = 0,
    PROCESS_STATUS_ABSENT,
    PROCESS_STATUS_NULL_ARGUMENT,
    PROCESS_STATUS_BUSY,
    PROCESS_STATUS_PREREQUISITE,
    PROCESS_STATUS_FILESYSTEM,
    PROCESS_STATUS_ELF_PARSER,
    PROCESS_STATUS_ELF_PLACEMENT,
    PROCESS_STATUS_FRAME_ALLOCATION,
    PROCESS_STATUS_FRAME_INITIALIZATION,
    PROCESS_STATUS_ADDRESS_SPACE,
    PROCESS_STATUS_IMAGE_ALIAS,
    PROCESS_STATUS_USER_MAPPING,
    PROCESS_STATUS_USER_WALK,
    PROCESS_STATUS_CPU_CONTRACT,
    PROCESS_STATUS_GATE,
    PROCESS_STATUS_ENTRY,
    PROCESS_STATUS_RETURN_AUTHENTICATION,
    PROCESS_STATUS_KERNEL_CR3,
    PROCESS_STATUS_TRANSITION_REPEATED,
    PROCESS_STATUS_TRANSITION_REVERSED,
    PROCESS_STATUS_TRANSITION_INVALID,
    PROCESS_STATUS_TEARDOWN,
    PROCESS_STATUS_RESOURCE_CENSUS,
    PROCESS_STATUS_SENTINEL,
    PROCESS_STATUS_ROBUSTNESS,
    PROCESS_STATUS_COUNT
};

struct process_proof_result {
    uint32_t file_bytes;
    uint32_t segment_count;
    uint32_t result;
    uint32_t robustness_tests;
    bool ring_three;
    bool private_address_space;
    bool image_read_execute;
    bool stack_read_write_no_execute;
    bool guard_unmapped;
    bool interrupt_authenticated;
    bool normal_exit;
    bool teardown_complete;
    bool resource_census_equal;
};

bool process_address_space_foundation_self_test(size_t *completed_tests);
bool process_elf64_foundation_self_test(size_t *completed_tests);
enum process_status process_installed_prove(
    struct process_proof_result *result
);
struct process_proof_result process_get_proof_result(void);
bool process_resources_released(void);
const char *process_status_string(enum process_status status);

/*
 * The complete CPL3 register set one bounded user process owns while it is not
 * running. It exists because a scheduler that suspends a process has to give
 * it back exactly what it had: the Ring 3 proof entered a program once and
 * never resumed it, so a bare entry and stack were enough.
 *
 * The field order is the order src/arch/x86_64/process.S loads them in, and
 * process_user_context_layout_self_test refuses to boot if the two disagree.
 */
struct process_user_context {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
};

bool process_user_context_layout_self_test(void);

/* Reviewed assembly boundary: returns only through the private proof interrupt. */
void process_enter_user(uint64_t entry, uint64_t stack_pointer);
void process_enter_user_context(const struct process_user_context *context);
uintptr_t process_user_resume_stack(void);
bool process_user_boundary_active(void);
void process_user_resume_from_interrupt(void);

#endif
