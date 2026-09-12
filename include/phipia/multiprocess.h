/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_MULTIPROCESS_H
#define PHIPIA_MULTIPROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/elf64.h>
#include <phipia/paging.h>

/*
 * How many bounded user processes exist at once. Every one of them owns a
 * complete private hierarchy, so this is the same bound paging.c publishes;
 * stating it here as a separate name would let the two drift.
 */
#define MULTIPROCESS_MAX_PROCESSES PAGING_PROCESS_SPACE_SLOTS

/*
 * How many times each process publishes its progress and hands the processor
 * back before it exits. Two would prove a switch happened; six proves the
 * scheduler keeps coming back to the same processes in the same order after
 * one of them has already left, which is the property a round-robin actually
 * has to have.
 */
#define MULTIPROCESS_ROUNDS 6U

/*
 * The exact multiprocess executable. It is a second admitted profile of the
 * same ELF64 subset the Ring 3 proof uses - one read-execute load segment, one
 * page, no section table - with a longer body, because a program that yields
 * does not fit in the proof executable's eight instruction bytes.
 */
#define MULTIPROCESS_ELF_FILE_BYTES 256U
#define MULTIPROCESS_ELF_CODE_OFFSET 120U
#define MULTIPROCESS_ELF_CODE_BYTES 136U
#define MULTIPROCESS_ENTRY_ADDRESS \
    (PAGING_PROCESS_IMAGE_ADDRESS + MULTIPROCESS_ELF_CODE_OFFSET)

/* Where the program is when the kernel takes it back, and why. */
#define MULTIPROCESS_YIELD_RETURN_ADDRESS \
    (MULTIPROCESS_ENTRY_ADDRESS + UINT64_C(0x1F))
#define MULTIPROCESS_EXIT_RETURN_ADDRESS \
    (MULTIPROCESS_ENTRY_ADDRESS + UINT64_C(0x2E))
#define MULTIPROCESS_FAULT_ADDRESS \
    (MULTIPROCESS_ENTRY_ADDRESS + UINT64_C(0x3A))
#define MULTIPROCESS_YIELD_RESULT UINT32_C(0x5341504D)
#define MULTIPROCESS_EXIT_RESULT UINT32_C(0x53415058)

/* Where the program publishes what it has reached, on its own stack. */
#define MULTIPROCESS_PROGRESS_ADDRESS (PAGING_PROCESS_STACK_END - UINT64_C(8))
#define MULTIPROCESS_IDENTITY_ADDRESS (PAGING_PROCESS_STACK_END - UINT64_C(16))

/*
 * The identity the kernel hands process N. Distinct per process and never
 * zero, so a stack page carrying another process's identity is a visible
 * isolation failure rather than an ambiguous one.
 */
#define MULTIPROCESS_IDENTITY_BASE UINT64_C(0x5341504F54450000)
#define MULTIPROCESS_IDENTITY(index) \
    (MULTIPROCESS_IDENTITY_BASE + (uint64_t)(index) + UINT64_C(1))

/* Every switch the clean schedule makes: one per round, plus one exit each. */
#define MULTIPROCESS_EXPECTED_SWITCHES \
    (MULTIPROCESS_MAX_PROCESSES * (MULTIPROCESS_ROUNDS + 1U))

/* Room for the recorded schedule, with slack so an overrun is a status. */
#define MULTIPROCESS_SWITCH_CAPACITY (MULTIPROCESS_EXPECTED_SWITCHES + 8U)

/* Controlled build-failure points swept before the proof runs for real. */
#define MULTIPROCESS_CONTROLLED_ROBUSTNESS_TESTS 8U

enum multiprocess_process_state {
    MULTIPROCESS_PROCESS_ABSENT = 0,
    MULTIPROCESS_PROCESS_BUILDING,
    MULTIPROCESS_PROCESS_RUNNABLE,
    MULTIPROCESS_PROCESS_FINISHED,
    MULTIPROCESS_PROCESS_TERMINATED,
    MULTIPROCESS_PROCESS_RELEASED,
    MULTIPROCESS_PROCESS_STATE_COUNT
};

enum multiprocess_trap {
    MULTIPROCESS_TRAP_NONE = 0,
    MULTIPROCESS_TRAP_YIELD,
    MULTIPROCESS_TRAP_EXIT,
    MULTIPROCESS_TRAP_FAULT,
    MULTIPROCESS_TRAP_UNEXPECTED,
    MULTIPROCESS_TRAP_COUNT
};

enum multiprocess_status {
    MULTIPROCESS_STATUS_OK = 0,
    MULTIPROCESS_STATUS_NULL_ARGUMENT,
    MULTIPROCESS_STATUS_BUSY,
    MULTIPROCESS_STATUS_PREREQUISITE,
    MULTIPROCESS_STATUS_ELF_PARSER,
    MULTIPROCESS_STATUS_ELF_PLACEMENT,
    MULTIPROCESS_STATUS_FRAME_ALLOCATION,
    MULTIPROCESS_STATUS_FRAME_INITIALIZATION,
    MULTIPROCESS_STATUS_ADDRESS_SPACE,
    MULTIPROCESS_STATUS_IMAGE_ALIAS,
    MULTIPROCESS_STATUS_USER_MAPPING,
    MULTIPROCESS_STATUS_USER_WALK,
    MULTIPROCESS_STATUS_CPU_CONTRACT,
    MULTIPROCESS_STATUS_GATE,
    MULTIPROCESS_STATUS_ENTRY,
    MULTIPROCESS_STATUS_CONTEXT_AUTHENTICATION,
    MULTIPROCESS_STATUS_TRAP_AUTHENTICATION,
    MULTIPROCESS_STATUS_KERNEL_CR3,
    MULTIPROCESS_STATUS_SCHEDULE,
    MULTIPROCESS_STATUS_ISOLATION,
    MULTIPROCESS_STATUS_CONTAINMENT,
    MULTIPROCESS_STATUS_TEARDOWN,
    MULTIPROCESS_STATUS_RESOURCE_CENSUS,
    MULTIPROCESS_STATUS_SENTINEL,
    MULTIPROCESS_STATUS_ROBUSTNESS,
    MULTIPROCESS_STATUS_COUNT
};

struct multiprocess_proof_result {
    uint32_t process_count;
    uint32_t rounds;
    uint32_t switches;
    uint32_t completed;
    uint32_t terminated;
    uint32_t address_space_table_frames;
    uint32_t robustness_tests;
    bool concurrent_address_spaces;
    bool round_robin_interleaved;
    bool contexts_preserved;
    bool isolation_confirmed;
    bool fault_contained;
    bool teardown_complete;
    bool resource_census_equal;
};

bool multiprocess_foundation_self_test(size_t *completed_tests);
enum multiprocess_status multiprocess_prove(
    struct multiprocess_proof_result *result
);
struct multiprocess_proof_result multiprocess_get_proof_result(void);
bool multiprocess_resources_released(void);
const char *multiprocess_status_string(enum multiprocess_status status);
const char *multiprocess_trap_string(enum multiprocess_trap trap);

uint32_t phipia_multiprocess_elf64_self_test(void);
enum elf64_status phipia_multiprocess_elf64_parse(
    const uint8_t *input,
    size_t input_len,
    struct elf64_validated_image *out
);

#endif
