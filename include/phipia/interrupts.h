/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_INTERRUPTS_H
#define PHIPIA_INTERRUPTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTERRUPT_VECTOR_COUNT 256U
#define INTERRUPT_EXCEPTION_COUNT 32U
#define INTERRUPT_PIC_MASTER_BASE 32U
#define INTERRUPT_PIC_SLAVE_BASE 40U
#define INTERRUPT_PIC_LIMIT 48U

/*
 * Vectors the I/O APIC delivers, one per ISA interrupt. They are deliberately
 * disjoint from the 8259 range so the dispatcher can tell from the vector alone
 * which controller must be acknowledged.
 */
#define INTERRUPT_IOAPIC_BASE 48U
#define INTERRUPT_IOAPIC_LIMIT 64U

/*
 * Vectors the local APIC raises for its own sources, starting with the timer.
 * They are acknowledged the same way as I/O APIC vectors, so the two ranges are
 * kept adjacent and the dispatcher tests them as one span.
 */
#define INTERRUPT_LOCAL_APIC_BASE 64U
#define INTERRUPT_LOCAL_APIC_LIMIT 72U
#define INTERRUPT_UNEXPECTED_TEST_VECTOR UINT8_C(0x80)
#define INTERRUPT_PROCESS_PROOF_VECTOR UINT8_C(0x81)
#define INTERRUPT_DYNAMIC_BASE UINT8_C(0x90)
#define INTERRUPT_DYNAMIC_LIMIT UINT8_C(0xF0)
#define INTERRUPT_IST_TEST_VECTOR UINT8_C(0xF0)

struct interrupt_frame {
    uint64_t cr2;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
};

/* Present only after privilege change/IST; use interrupt_frame_has_stack_tail. */
struct interrupt_stack_tail {
    uint64_t rsp;
    uint64_t ss;
};

typedef void (*interrupt_handler_t)(
    struct interrupt_frame *frame,
    void *context
);

enum interrupt_status {
    INTERRUPT_STATUS_OK = 0,
    INTERRUPT_STATUS_ALREADY_INITIALIZED,
    INTERRUPT_STATUS_NOT_INITIALIZED,
    INTERRUPT_STATUS_CPU_TABLE_FAILURE,
    INTERRUPT_STATUS_PIC_FAILURE,
    INTERRUPT_STATUS_BAD_IDT,
    INTERRUPT_STATUS_INTERRUPTS_ENABLED,
    INTERRUPT_STATUS_HANDLER_PRESENT,
    INTERRUPT_STATUS_HANDLER_MISSING,
    INTERRUPT_STATUS_RESERVED_VECTOR,
    INTERRUPT_STATUS_NULL_HANDLER,
    INTERRUPT_STATUS_BAD_ARGUMENT,
    INTERRUPT_STATUS_PROOF_GATE_BUSY,
    INTERRUPT_STATUS_PROOF_GATE_BAD_TOKEN,
    INTERRUPT_STATUS_PROOF_GATE_BAD_STATE,
    INTERRUPT_STATUS_PROOF_GATE_BAD_DESCRIPTOR,
    INTERRUPT_STATUS_PROOF_GATE_BAD_FRAME,
    INTERRUPT_STATUS_PROOF_GATE_BAD_RESUME
};

enum interrupt_process_gate_state {
    INTERRUPT_PROCESS_GATE_INACTIVE = 0,
    INTERRUPT_PROCESS_GATE_ARMED,
    INTERRUPT_PROCESS_GATE_ENTERED,
    INTERRUPT_PROCESS_GATE_RETURNED,
    INTERRUPT_PROCESS_GATE_DISARMED,
    INTERRUPT_PROCESS_GATE_STATE_COUNT
};

struct interrupt_process_gate {
    uint64_t generation;
    enum interrupt_process_gate_state state;
    bool active;
};

enum interrupt_status interrupts_initialize(void);
enum interrupt_status interrupts_validate(void);
bool interrupts_ready(void);
enum interrupt_status interrupt_register_handler(
    uint8_t vector,
    interrupt_handler_t handler,
    void *context
);
enum interrupt_status interrupt_unregister_handler(uint8_t vector);
enum interrupt_status interrupt_process_gate_arm(
    interrupt_handler_t handler,
    void *context,
    struct interrupt_process_gate *gate
);
enum interrupt_status interrupt_process_gate_validate(
    struct interrupt_process_gate *gate
);
enum interrupt_status interrupt_process_gate_disarm(
    struct interrupt_process_gate *gate
);
/*
 * Return an entered-and-returned gate to armed without touching the interrupt
 * descriptor. The Ring 3 proof entered CPL3 once and tore the gate down, so it
 * never needed this; a scheduler that gives the processor back to a suspended
 * process crosses the same gate on every switch, and re-arming through disarm
 * and arm would rewrite the descriptor on every one of them.
 */
enum interrupt_status interrupt_process_gate_rearm(
    struct interrupt_process_gate *gate
);
enum interrupt_status interrupt_request_kernel_resume(
    const struct interrupt_frame *frame,
    uintptr_t resume_stack
);
bool interrupt_process_gate_resources_released(void);
bool interrupt_frame_has_stack_tail(const struct interrupt_frame *frame);
uintptr_t interrupt_frame_stack_pointer(const struct interrupt_frame *frame);
uint16_t interrupt_frame_stack_selector(const struct interrupt_frame *frame);
/* Assembly-only: zero selects IRETQ; nonzero is the authenticated resume RSP. */
uintptr_t interrupt_dispatch(struct interrupt_frame *frame);
const char *interrupt_status_string(enum interrupt_status status);
const char *interrupt_exception_name(uint8_t vector);
void interrupt_test_set_gate_present(uint8_t vector, bool present);

bool interrupt_breakpoint_self_test(void);
bool interrupt_ist_self_test(void);
bool interrupt_pic_spurious_self_test(void);
bool interrupt_frame_layout_self_test(void);
bool interrupt_trigger_register_probe(void);
void interrupt_trigger_ist_probe(void);
void interrupt_trigger_spurious_irq7(void);
void interrupt_trigger_spurious_irq15(void);
_Noreturn void interrupt_trigger_invalid_opcode(void);
_Noreturn void interrupt_trigger_page_fault(void);
_Noreturn void interrupt_trigger_unexpected(void);

extern const uintptr_t interrupt_vector_table[INTERRUPT_VECTOR_COUNT];
extern const uint8_t interrupt_vector_table_end[];
extern const uint8_t interrupt_invalid_opcode_site[];
extern const uint8_t interrupt_page_fault_site[];

#endif
