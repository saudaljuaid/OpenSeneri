/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/apic.h>
#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/interrupts.h>
#include <phipia/ioapic.h>
#include <phipia/interrupt_vector.h>
#include <phipia/native_process.h>
#include <phipia/pic.h>
#include <phipia/thread.h>
#include <phipia/test.h>

#define IDT_GATE_PRESENT UINT8_C(0x80)
#define IDT_GATE_INTERRUPT UINT8_C(0x0E)
#define IDT_GATE_DPL_ZERO UINT8_C(0x00)
#define IDT_GATE_DPL_THREE UINT8_C(0x60)
#define IDT_GATE_ATTRIBUTES \
    (IDT_GATE_PRESENT | IDT_GATE_DPL_ZERO | IDT_GATE_INTERRUPT)
#define IDT_IST_MASK UINT8_C(0x07)
#define PAGE_FAULT_PRESENT UINT64_C(1)
#define PAGE_FAULT_WRITE UINT64_C(2)
#define PAGE_FAULT_USER UINT64_C(4)
#define PAGE_FAULT_RESERVED UINT64_C(8)
#define PAGE_FAULT_INSTRUCTION UINT64_C(16)

struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct handler_slot {
    interrupt_handler_t handler;
    void *context;
};

_Static_assert(sizeof(struct idt_gate) == 16U, "IDT gate must be sixteen bytes");
_Static_assert(sizeof(struct interrupt_frame) == 168U,
               "interrupt frame ABI size changed");
_Static_assert(offsetof(struct interrupt_frame, cr2) == 0U,
               "interrupt CR2 offset changed");
_Static_assert(offsetof(struct interrupt_frame, r15) == 8U,
               "interrupt R15 offset changed");
_Static_assert(offsetof(struct interrupt_frame, rsi) == 72U,
               "interrupt RSI offset changed");
_Static_assert(offsetof(struct interrupt_frame, rax) == 120U,
               "interrupt RAX offset changed");
_Static_assert(offsetof(struct interrupt_frame, vector) == 128U,
               "interrupt vector offset changed");
_Static_assert(offsetof(struct interrupt_frame, error_code) == 136U,
               "interrupt error-code offset changed");
_Static_assert(offsetof(struct interrupt_frame, rip) == 144U,
               "interrupt RIP offset changed");
_Static_assert(offsetof(struct interrupt_frame, rflags) == 160U,
               "interrupt RFLAGS offset changed");
_Static_assert(sizeof(struct interrupt_stack_tail) == 16U,
               "interrupt privilege tail ABI size changed");

static struct idt_gate idt[INTERRUPT_VECTOR_COUNT] __attribute__((aligned(16)));
static struct handler_slot handlers[INTERRUPT_VECTOR_COUNT];
static struct descriptor_table_pointer idt_pointer;
static bool initialized;
static volatile unsigned int fatal_depth;

struct process_gate_runtime {
    uint64_t generation;
    enum interrupt_process_gate_state state;
    const struct interrupt_frame *dispatch_frame;
    uintptr_t resume_stack;
    bool owned;
};

static struct process_gate_runtime process_gate_runtime;
static uint64_t next_process_gate_generation = UINT64_C(1);

static const char *const exception_names[INTERRUPT_EXCEPTION_COUNT] = {
    "divide error",
    "debug",
    "non-maskable interrupt",
    "breakpoint",
    "overflow",
    "bound-range exceeded",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor segment overrun",
    "invalid TSS",
    "segment not present",
    "stack-segment fault",
    "general-protection fault",
    "page fault",
    "reserved exception 15",
    "x87 floating-point exception",
    "alignment check",
    "machine check",
    "SIMD floating-point exception",
    "virtualization exception",
    "control-protection exception",
    "reserved exception 22",
    "reserved exception 23",
    "reserved exception 24",
    "reserved exception 25",
    "reserved exception 26",
    "reserved exception 27",
    "hypervisor-injection exception",
    "VMM communication exception",
    "security exception",
    "reserved exception 31"
};

static uintptr_t idt_gate_offset(const struct idt_gate *gate)
{
    uint64_t offset = gate->offset_low;

    offset |= (uint64_t)gate->offset_middle << 16U;
    offset |= (uint64_t)gate->offset_high << 32U;
    return (uintptr_t)offset;
}

static void idt_set_gate(
    uint8_t vector,
    uintptr_t handler,
    uint8_t ist,
    bool present
)
{
    struct idt_gate *gate = &idt[vector];

    gate->offset_low = (uint16_t)(handler & UINT64_C(0xFFFF));
    gate->selector = CPU_GDT_CODE_SELECTOR;
    gate->ist = ist & IDT_IST_MASK;
    gate->type_attributes = IDT_GATE_INTERRUPT | IDT_GATE_DPL_ZERO;

    if (present) {
        gate->type_attributes |= IDT_GATE_PRESENT;
    }

    gate->offset_middle = (uint16_t)((handler >> 16U) & UINT64_C(0xFFFF));
    gate->offset_high = (uint32_t)((handler >> 32U) & UINT64_C(0xFFFFFFFF));
    gate->reserved = 0U;
}

static uint8_t expected_ist(uint8_t vector)
{
    if (vector == 8U || vector == INTERRUPT_IST_TEST_VECTOR) {
        return CPU_IST_DOUBLE_FAULT;
    }

    if (vector == 2U) {
        return CPU_IST_NMI;
    }

    if (vector == 18U) {
        return CPU_IST_MACHINE_CHECK;
    }

    return 0U;
}

bool interrupt_frame_has_stack_tail(const struct interrupt_frame *frame)
{
    return frame != NULL && (((frame->cs & UINT64_C(3)) != 0U) ||
        (frame->vector < INTERRUPT_VECTOR_COUNT &&
            expected_ist((uint8_t)frame->vector) != 0U));
}

uintptr_t interrupt_frame_stack_pointer(const struct interrupt_frame *frame)
{
    const struct interrupt_stack_tail *tail;

    if (frame == NULL) {
        return 0U;
    }
    if (!interrupt_frame_has_stack_tail(frame)) {
        return (uintptr_t)(const void *)frame + sizeof(*frame);
    }
    tail = (const struct interrupt_stack_tail *)(const void *)
        ((const uint8_t *)(const void *)frame + sizeof(*frame));
    return (uintptr_t)tail->rsp;
}

uint16_t interrupt_frame_stack_selector(const struct interrupt_frame *frame)
{
    const struct interrupt_stack_tail *tail;

    if (frame == NULL) {
        return 0U;
    }
    if (!interrupt_frame_has_stack_tail(frame)) {
        return CPU_GDT_DATA_SELECTOR;
    }
    tail = (const struct interrupt_stack_tail *)(const void *)
        ((const uint8_t *)(const void *)frame + sizeof(*frame));
    return (uint16_t)tail->ss;
}

static bool process_gate_descriptor_valid(void)
{
    const struct idt_gate *gate = &idt[INTERRUPT_PROCESS_PROOF_VECTOR];

    return idt_gate_offset(gate) ==
            interrupt_vector_table[INTERRUPT_PROCESS_PROOF_VECTOR] &&
        gate->selector == CPU_GDT_CODE_SELECTOR && gate->ist == 0U &&
        gate->type_attributes ==
            (IDT_GATE_PRESENT | IDT_GATE_DPL_THREE | IDT_GATE_INTERRUPT) &&
        gate->reserved == 0U && cpu_user_transition_contract_valid() &&
        cpu_tss_rsp0() != 0U;
}

static enum interrupt_status validate_idt(void)
{
    struct descriptor_table_pointer observed;

    cpu_store_idt(&observed);

    if (observed.limit != idt_pointer.limit || observed.base != idt_pointer.base) {
        return INTERRUPT_STATUS_BAD_IDT;
    }

    if ((uintptr_t)(const void *)interrupt_vector_table_end -
        (uintptr_t)(const void *)interrupt_vector_table !=
        INTERRUPT_VECTOR_COUNT * sizeof(uintptr_t)) {
        return INTERRUPT_STATUS_BAD_IDT;
    }

    for (size_t vector = 0; vector < INTERRUPT_VECTOR_COUNT; ++vector) {
        const struct idt_gate *gate = &idt[vector];

        const uint8_t expected_attributes = process_gate_runtime.owned &&
            vector == INTERRUPT_PROCESS_PROOF_VECTOR ?
            (IDT_GATE_PRESENT | IDT_GATE_DPL_THREE | IDT_GATE_INTERRUPT) :
            IDT_GATE_ATTRIBUTES;

        if (idt_gate_offset(gate) != interrupt_vector_table[vector] ||
            gate->selector != CPU_GDT_CODE_SELECTOR ||
            gate->ist != expected_ist((uint8_t)vector) ||
            gate->type_attributes != expected_attributes ||
            gate->reserved != 0U) {
            return INTERRUPT_STATUS_BAD_IDT;
        }
    }

    return INTERRUPT_STATUS_OK;
}

static void report_page_fault(uint64_t error_code)
{
    console_write("  page-fault bits: P=");
    console_write_u64((error_code & PAGE_FAULT_PRESENT) != 0U);
    console_write(" W=");
    console_write_u64((error_code & PAGE_FAULT_WRITE) != 0U);
    console_write(" U=");
    console_write_u64((error_code & PAGE_FAULT_USER) != 0U);
    console_write(" RSVD=");
    console_write_u64((error_code & PAGE_FAULT_RESERVED) != 0U);
    console_write(" I=");
    console_write_u64((error_code & PAGE_FAULT_INSTRUCTION) != 0U);
    console_putc('\n');
}

static void report_register(const char *name, uint64_t value)
{
    console_write("  ");
    console_write(name);
    console_write("=");
    console_write_hex(value);
    console_putc('\n');
}

static _Noreturn void fatal_interrupt(struct interrupt_frame *frame)
{
    cpu_interrupt_disable();

    if (fatal_depth != 0U) {
        console_halt();
    }

    fatal_depth = 1U;
    console_write("Phipia FATAL INTERRUPT\n");
    console_write("  vector=");
    console_write_u64(frame->vector);
    console_write(" name=");
    console_write(interrupt_exception_name((uint8_t)frame->vector));
    console_putc('\n');
    report_register("error", frame->error_code);
    report_register("rip", frame->rip);
    report_register("cs", frame->cs);
    report_register("rflags", frame->rflags);
    report_register("rsp", interrupt_frame_stack_pointer(frame));
    report_register("ss", interrupt_frame_stack_selector(frame));
    report_register("cr2", frame->cr2);
    report_register("rax", frame->rax);
    report_register("rbx", frame->rbx);
    report_register("rcx", frame->rcx);
    report_register("rdx", frame->rdx);
    report_register("rsi", frame->rsi);
    report_register("rdi", frame->rdi);
    report_register("rbp", frame->rbp);
    report_register("r8", frame->r8);
    report_register("r9", frame->r9);
    report_register("r10", frame->r10);
    report_register("r11", frame->r11);
    report_register("r12", frame->r12);
    report_register("r13", frame->r13);
    report_register("r14", frame->r14);
    report_register("r15", frame->r15);

    if (frame->vector == 14U) {
        report_page_fault(frame->error_code);
    }

    (void)kernel_test_handle_fatal_interrupt(frame);
    console_halt();
}

enum interrupt_status interrupts_initialize(void)
{
    enum cpu_status cpu_status;
    enum pic_status pic_status;
    enum interrupt_status validation_status;

    if (initialized) {
        return INTERRUPT_STATUS_ALREADY_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return INTERRUPT_STATUS_INTERRUPTS_ENABLED;
    }

    pic_mask_all_early();
    cpu_status = cpu_tables_prepare();

    if (cpu_status != CPU_STATUS_OK) {
        return INTERRUPT_STATUS_CPU_TABLE_FAILURE;
    }

    for (size_t vector = 0; vector < INTERRUPT_VECTOR_COUNT; ++vector) {
        idt_set_gate((uint8_t)vector, interrupt_vector_table[vector], 0U, true);
        handlers[vector].handler = NULL;
        handlers[vector].context = NULL;
    }

    idt_pointer.limit = (uint16_t)(sizeof(idt) - 1U);
    idt_pointer.base = (uintptr_t)(void *)idt;

    /* A provisional IST-free IDT makes LGDT/LTR failures observable. */
    cpu_load_idt(&idt_pointer);
    cpu_status = cpu_tables_activate();

    if (cpu_status != CPU_STATUS_OK) {
        return INTERRUPT_STATUS_CPU_TABLE_FAILURE;
    }

    idt_set_gate(8U, interrupt_vector_table[8], CPU_IST_DOUBLE_FAULT, true);
    idt_set_gate(2U, interrupt_vector_table[2], CPU_IST_NMI, true);
    idt_set_gate(18U, interrupt_vector_table[18], CPU_IST_MACHINE_CHECK, true);
    idt_set_gate(
        INTERRUPT_IST_TEST_VECTOR,
        interrupt_vector_table[INTERRUPT_IST_TEST_VECTOR],
        CPU_IST_DOUBLE_FAULT,
        true
    );

    validation_status = validate_idt();

    if (validation_status != INTERRUPT_STATUS_OK ||
        cpu_tables_validate() != CPU_STATUS_OK) {
        return INTERRUPT_STATUS_BAD_IDT;
    }

    pic_status = pic_initialize();

    if (pic_status != PIC_STATUS_OK) {
        return INTERRUPT_STATUS_PIC_FAILURE;
    }

    initialized = true;
    return INTERRUPT_STATUS_OK;
}

enum interrupt_status interrupts_validate(void)
{
    if (!initialized) {
        return INTERRUPT_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return INTERRUPT_STATUS_INTERRUPTS_ENABLED;
    }

    if (cpu_tables_validate() != CPU_STATUS_OK || !cpu_stack_canaries_valid()) {
        return INTERRUPT_STATUS_CPU_TABLE_FAILURE;
    }

    return validate_idt();
}

bool interrupts_ready(void)
{
    return initialized;
}

enum interrupt_status interrupt_register_handler(
    uint8_t vector,
    interrupt_handler_t handler,
    void *context
)
{
    if (!initialized) {
        return INTERRUPT_STATUS_NOT_INITIALIZED;
    }

    if (handler == NULL) {
        return INTERRUPT_STATUS_NULL_HANDLER;
    }

    if (cpu_interrupts_enabled()) {
        return INTERRUPT_STATUS_INTERRUPTS_ENABLED;
    }

    if (vector == 2U || vector == 8U || vector == 18U ||
        vector == INTERRUPT_PROCESS_PROOF_VECTOR) {
        return INTERRUPT_STATUS_RESERVED_VECTOR;
    }

    if (handlers[vector].handler != NULL) {
        return INTERRUPT_STATUS_HANDLER_PRESENT;
    }

    handlers[vector].context = context;
    __asm__ volatile ("" : : : "memory");
    handlers[vector].handler = handler;
    return INTERRUPT_STATUS_OK;
}

enum interrupt_status interrupt_unregister_handler(uint8_t vector)
{
    if (!initialized) {
        return INTERRUPT_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return INTERRUPT_STATUS_INTERRUPTS_ENABLED;
    }

    if (vector == 2U || vector == 8U || vector == 18U ||
        vector == INTERRUPT_PROCESS_PROOF_VECTOR) {
        return INTERRUPT_STATUS_RESERVED_VECTOR;
    }

    if (handlers[vector].handler == NULL) {
        return INTERRUPT_STATUS_HANDLER_MISSING;
    }

    handlers[vector].handler = NULL;
    __asm__ volatile ("" : : : "memory");
    handlers[vector].context = NULL;
    return INTERRUPT_STATUS_OK;
}

enum interrupt_status interrupt_process_gate_arm(
    interrupt_handler_t handler,
    void *context,
    struct interrupt_process_gate *gate
)
{
    if (gate == NULL || handler == NULL) {
        return handler == NULL ? INTERRUPT_STATUS_NULL_HANDLER :
            INTERRUPT_STATUS_BAD_ARGUMENT;
    }
    gate->generation = 0U;
    gate->state = INTERRUPT_PROCESS_GATE_INACTIVE;
    gate->active = false;
    if (!initialized) {
        return INTERRUPT_STATUS_NOT_INITIALIZED;
    }
    if (cpu_interrupts_enabled()) {
        return INTERRUPT_STATUS_INTERRUPTS_ENABLED;
    }
    if (process_gate_runtime.owned ||
        handlers[INTERRUPT_PROCESS_PROOF_VECTOR].handler != NULL) {
        return INTERRUPT_STATUS_PROOF_GATE_BUSY;
    }
    handlers[INTERRUPT_PROCESS_PROOF_VECTOR].context = context;
    __asm__ volatile ("" : : : "memory");
    handlers[INTERRUPT_PROCESS_PROOF_VECTOR].handler = handler;
    idt[INTERRUPT_PROCESS_PROOF_VECTOR].type_attributes =
        IDT_GATE_PRESENT | IDT_GATE_DPL_THREE | IDT_GATE_INTERRUPT;
    process_gate_runtime.generation = next_process_gate_generation++;
    if (next_process_gate_generation == 0U) {
        next_process_gate_generation = 1U;
    }
    process_gate_runtime.state = INTERRUPT_PROCESS_GATE_ARMED;
    process_gate_runtime.dispatch_frame = NULL;
    process_gate_runtime.resume_stack = 0U;
    process_gate_runtime.owned = true;
    if (!process_gate_descriptor_valid()) {
        idt[INTERRUPT_PROCESS_PROOF_VECTOR].type_attributes =
            IDT_GATE_ATTRIBUTES;
        handlers[INTERRUPT_PROCESS_PROOF_VECTOR].handler = NULL;
        handlers[INTERRUPT_PROCESS_PROOF_VECTOR].context = NULL;
        process_gate_runtime.owned = false;
        return INTERRUPT_STATUS_PROOF_GATE_BAD_DESCRIPTOR;
    }
    gate->generation = process_gate_runtime.generation;
    gate->state = process_gate_runtime.state;
    gate->active = true;
    return INTERRUPT_STATUS_OK;
}

enum interrupt_status interrupt_process_gate_validate(
    struct interrupt_process_gate *gate
)
{
    if (gate == NULL) {
        return INTERRUPT_STATUS_BAD_ARGUMENT;
    }
    if (!gate->active || !process_gate_runtime.owned ||
        gate->generation != process_gate_runtime.generation) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_TOKEN;
    }
    if (!process_gate_descriptor_valid()) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_DESCRIPTOR;
    }
    gate->state = process_gate_runtime.state;
    return INTERRUPT_STATUS_OK;
}

enum interrupt_status interrupt_process_gate_disarm(
    struct interrupt_process_gate *gate
)
{
    if (gate == NULL) {
        return INTERRUPT_STATUS_BAD_ARGUMENT;
    }
    if (!gate->active || !process_gate_runtime.owned ||
        gate->generation != process_gate_runtime.generation) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_TOKEN;
    }
    if (cpu_interrupts_enabled()) {
        return INTERRUPT_STATUS_INTERRUPTS_ENABLED;
    }
    if (process_gate_runtime.state != INTERRUPT_PROCESS_GATE_ARMED &&
        process_gate_runtime.state != INTERRUPT_PROCESS_GATE_RETURNED) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_STATE;
    }
    idt[INTERRUPT_PROCESS_PROOF_VECTOR].type_attributes = IDT_GATE_ATTRIBUTES;
    handlers[INTERRUPT_PROCESS_PROOF_VECTOR].handler = NULL;
    __asm__ volatile ("" : : : "memory");
    handlers[INTERRUPT_PROCESS_PROOF_VECTOR].context = NULL;
    process_gate_runtime.state = INTERRUPT_PROCESS_GATE_DISARMED;
    process_gate_runtime.dispatch_frame = NULL;
    process_gate_runtime.resume_stack = 0U;
    process_gate_runtime.owned = false;
    gate->state = INTERRUPT_PROCESS_GATE_DISARMED;
    gate->active = false;
    return INTERRUPT_STATUS_OK;
}

enum interrupt_status interrupt_process_gate_rearm(
    struct interrupt_process_gate *gate
)
{
    if (gate == NULL) {
        return INTERRUPT_STATUS_BAD_ARGUMENT;
    }
    if (!gate->active || !process_gate_runtime.owned ||
        gate->generation != process_gate_runtime.generation) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_TOKEN;
    }
    if (cpu_interrupts_enabled()) {
        return INTERRUPT_STATUS_INTERRUPTS_ENABLED;
    }
    /*
     * Only the state moves. A gate that never entered CPL3, or that is still
     * inside a dispatch, is not something a caller may declare ready again.
     */
    if (process_gate_runtime.state != INTERRUPT_PROCESS_GATE_RETURNED ||
        process_gate_runtime.dispatch_frame != NULL ||
        handlers[INTERRUPT_PROCESS_PROOF_VECTOR].handler == NULL) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_STATE;
    }
    if (!process_gate_descriptor_valid()) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_DESCRIPTOR;
    }
    process_gate_runtime.resume_stack = 0U;
    process_gate_runtime.state = INTERRUPT_PROCESS_GATE_ARMED;
    gate->state = INTERRUPT_PROCESS_GATE_ARMED;
    return INTERRUPT_STATUS_OK;
}

enum interrupt_status interrupt_request_kernel_resume(
    const struct interrupt_frame *frame,
    uintptr_t resume_stack
)
{
    const uint64_t high = (uint64_t)resume_stack >> 47U;

    if (!process_gate_runtime.owned ||
        process_gate_runtime.state != INTERRUPT_PROCESS_GATE_ENTERED) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_STATE;
    }
    if (frame == NULL || frame != process_gate_runtime.dispatch_frame ||
        (frame->cs & UINT64_C(3)) != 3U ||
        !interrupt_frame_has_stack_tail(frame)) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_FRAME;
    }
    if (resume_stack == 0U || (resume_stack & (uintptr_t)0xFU) != 8U ||
        (high != 0U && high != UINT64_C(0x1FFFF)) ||
        process_gate_runtime.resume_stack != 0U) {
        return INTERRUPT_STATUS_PROOF_GATE_BAD_RESUME;
    }
    process_gate_runtime.resume_stack = resume_stack;
    return INTERRUPT_STATUS_OK;
}

bool interrupt_process_gate_resources_released(void)
{
    return !process_gate_runtime.owned &&
        handlers[INTERRUPT_PROCESS_PROOF_VECTOR].handler == NULL &&
        idt[INTERRUPT_PROCESS_PROOF_VECTOR].type_attributes ==
            IDT_GATE_ATTRIBUTES;
}

uintptr_t interrupt_dispatch(struct interrupt_frame *frame)
{
    uint8_t vector;
    struct handler_slot *slot;

    if (frame == NULL || frame->vector >= INTERRUPT_VECTOR_COUNT) {
        if (frame == NULL) {
            console_panic("null interrupt frame");
        }

        fatal_interrupt(frame);
    }

    vector = (uint8_t)frame->vector;
    slot = &handlers[vector];

    if ((frame->cs & UINT64_C(3)) != 0U) {
        struct handler_slot *proof_slot =
            &handlers[INTERRUPT_PROCESS_PROOF_VECTOR];
        uintptr_t resume_stack;

        if ((frame->cs & UINT64_C(3)) != 3U ||
            !process_gate_runtime.owned ||
            process_gate_runtime.state != INTERRUPT_PROCESS_GATE_ARMED ||
            proof_slot->handler == NULL || !interrupt_frame_has_stack_tail(frame)) {
            fatal_interrupt(frame);
        }
        /*
         * Native applications run with IF set, unlike the original one-shot
         * proof. Service and acknowledge a real device interrupt before the
         * native scheduler saves the userspace context. The scheduler return
         * replaces thread_on_interrupt_return for this boundary.
         */
        if (native_process_interrupt_active()) {
            if (vector >= INTERRUPT_PIC_MASTER_BASE &&
                vector < INTERRUPT_PIC_LIMIT) {
                const uint8_t irq =
                    (uint8_t)(vector - INTERRUPT_PIC_MASTER_BASE);

                if (!pic_irq_is_real(irq)) {
                    return 0U;
                }
                if (slot->handler == NULL) {
                    pic_send_eoi(irq);
                    fatal_interrupt(frame);
                }
                slot->handler(frame, slot->context);
                pic_send_eoi(irq);
            } else if (vector >= INTERRUPT_IOAPIC_BASE &&
                vector < INTERRUPT_LOCAL_APIC_LIMIT) {
                const bool level_triggered =
                    ioapic_vector_is_level_triggered(vector);

                if (slot->handler == NULL) {
                    if (level_triggered) {
                        if (ioapic_send_eoi(vector) != IOAPIC_STATUS_OK) {
                            console_panic("native I/O APIC EOI failed");
                        }
                    } else {
                        apic_send_eoi();
                    }
                    fatal_interrupt(frame);
                }
                slot->handler(frame, slot->context);
                if (level_triggered) {
                    if (ioapic_send_eoi(vector) != IOAPIC_STATUS_OK) {
                        console_panic("native I/O APIC EOI failed");
                    }
                } else {
                    apic_send_eoi();
                }
            } else if (vector >= INTERRUPT_DYNAMIC_BASE &&
                vector < INTERRUPT_DYNAMIC_LIMIT) {
                if (slot->handler == NULL ||
                    !interrupt_vector_is_allocated(vector)) {
                    apic_send_eoi();
                    fatal_interrupt(frame);
                }
                slot->handler(frame, slot->context);
                apic_send_eoi();
            } else if (vector >= INTERRUPT_EXCEPTION_COUNT &&
                vector != INTERRUPT_PROCESS_PROOF_VECTOR) {
                fatal_interrupt(frame);
            }
        }
        process_gate_runtime.state = INTERRUPT_PROCESS_GATE_ENTERED;
        process_gate_runtime.dispatch_frame = frame;
        process_gate_runtime.resume_stack = 0U;
        proof_slot->handler(frame, proof_slot->context);
        resume_stack = process_gate_runtime.resume_stack;
        if (resume_stack == 0U) {
            fatal_interrupt(frame);
        }
        process_gate_runtime.dispatch_frame = NULL;
        process_gate_runtime.state = INTERRUPT_PROCESS_GATE_RETURNED;
        return resume_stack;
    }
    if (vector == INTERRUPT_PROCESS_PROOF_VECTOR &&
        process_gate_runtime.owned) {
        fatal_interrupt(frame);
    }

    if (vector >= INTERRUPT_PIC_MASTER_BASE && vector < INTERRUPT_PIC_LIMIT) {
        const uint8_t irq = (uint8_t)(vector - INTERRUPT_PIC_MASTER_BASE);

        if (!pic_irq_is_real(irq)) {
            return 0U;
        }

        if (slot->handler == NULL) {
            pic_send_eoi(irq);
            fatal_interrupt(frame);
        }

        slot->handler(frame, slot->context);
        pic_send_eoi(irq);
        return 0U;
    }

    /*
     * Anything the APIC delivered, whether routed by an I/O APIC or raised by
     * the local APIC itself, is acknowledged at the local APIC rather than the
     * 8259 pair. The end of interrupt follows the handler so a second interrupt
     * from the same source cannot arrive while the first is still running.
     *
     * A level-triggered source is completed by ioapic_send_eoi. It observes the
     * live remote IRR, sends the local EOI, and—only when broadcasts have been
     * suppressed—follows it with the generating I/O APIC's directed EOI. That
     * is the ordering required by Intel SDM volume 3A section 13.8.5.
     */
    if (vector >= INTERRUPT_IOAPIC_BASE && vector < INTERRUPT_LOCAL_APIC_LIMIT) {
        const bool level_triggered = ioapic_vector_is_level_triggered(vector);
        enum ioapic_status ioapic_status;

        if (slot->handler == NULL) {
            if (level_triggered) {
                ioapic_status = ioapic_send_eoi(vector);

                if (ioapic_status != IOAPIC_STATUS_OK) {
                    console_panic(ioapic_status_string(ioapic_status));
                }
            } else {
                apic_send_eoi();
            }

            fatal_interrupt(frame);
        }

        slot->handler(frame, slot->context);
        if (level_triggered) {
            ioapic_status = ioapic_send_eoi(vector);

            if (ioapic_status != IOAPIC_STATUS_OK) {
                console_panic(ioapic_status_string(ioapic_status));
            }
        } else {
            apic_send_eoi();
        }

        /*
         * The one place a thread may be taken off the processor without asking.
         * It is after the acknowledgement on purpose: a scheduler that switched
         * away inside the handler would leave this interrupt in service at the
         * local APIC, and the next one would never arrive. A no-op unless a
         * quantum expired while this thread was running.
         */
        thread_on_interrupt_return();
        return 0U;
    }

    /*
     * MSI-X writes a dynamic vector directly into the local APIC. It never
     * traverses an I/O APIC redirection entry, so completion is exactly the
     * ordinary local-APIC EOI and never a directed I/O APIC EOI.
     */
    if (vector >= INTERRUPT_DYNAMIC_BASE &&
        vector < INTERRUPT_DYNAMIC_LIMIT) {
        if (slot->handler == NULL || !interrupt_vector_is_allocated(vector)) {
            apic_send_eoi();
            fatal_interrupt(frame);
        }

        slot->handler(frame, slot->context);
        apic_send_eoi();
        thread_on_interrupt_return();
        return 0U;
    }

    if (slot->handler != NULL) {
        slot->handler(frame, slot->context);
        return 0U;
    }

    fatal_interrupt(frame);
}

const char *interrupt_status_string(enum interrupt_status status)
{
    switch (status) {
    case INTERRUPT_STATUS_OK:
        return "ok";
    case INTERRUPT_STATUS_ALREADY_INITIALIZED:
        return "interrupt subsystem was initialized twice";
    case INTERRUPT_STATUS_NOT_INITIALIZED:
        return "interrupt subsystem is not initialized";
    case INTERRUPT_STATUS_CPU_TABLE_FAILURE:
        return "CPU descriptor table operation failed";
    case INTERRUPT_STATUS_PIC_FAILURE:
        return "PIC initialization failed";
    case INTERRUPT_STATUS_BAD_IDT:
        return "IDT validation failed";
    case INTERRUPT_STATUS_INTERRUPTS_ENABLED:
        return "interrupt mutation requires IF cleared";
    case INTERRUPT_STATUS_HANDLER_PRESENT:
        return "interrupt vector already has a handler";
    case INTERRUPT_STATUS_HANDLER_MISSING:
        return "interrupt vector has no handler";
    case INTERRUPT_STATUS_RESERVED_VECTOR:
        return "interrupt vector is reserved";
    case INTERRUPT_STATUS_NULL_HANDLER:
        return "interrupt handler is null";
    case INTERRUPT_STATUS_BAD_ARGUMENT:
        return "invalid interrupt argument";
    case INTERRUPT_STATUS_PROOF_GATE_BUSY:
        return "the private process proof gate is already owned";
    case INTERRUPT_STATUS_PROOF_GATE_BAD_TOKEN:
        return "the private process proof gate token is stale";
    case INTERRUPT_STATUS_PROOF_GATE_BAD_STATE:
        return "the private process proof gate is in the wrong state";
    case INTERRUPT_STATUS_PROOF_GATE_BAD_DESCRIPTOR:
        return "the private process proof gate descriptor is invalid";
    case INTERRUPT_STATUS_PROOF_GATE_BAD_FRAME:
        return "the private process proof interrupt frame is invalid";
    case INTERRUPT_STATUS_PROOF_GATE_BAD_RESUME:
        return "the private process proof resume stack is invalid";
    default:
        return "unknown interrupt status";
    }
}

const char *interrupt_exception_name(uint8_t vector)
{
    if (vector < INTERRUPT_EXCEPTION_COUNT) {
        return exception_names[vector];
    }

    if (vector < INTERRUPT_PIC_LIMIT) {
        return "legacy PIC interrupt";
    }

    return "unexpected vector";
}

void interrupt_test_set_gate_present(uint8_t vector, bool present)
{
    if (!initialized || cpu_interrupts_enabled()) {
        return;
    }

    if (present) {
        idt[vector].type_attributes |= IDT_GATE_PRESENT;
    } else {
        idt[vector].type_attributes &= (uint8_t)~IDT_GATE_PRESENT;
    }
}
