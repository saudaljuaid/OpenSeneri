/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/interrupts.h>
#include <phipia/pic.h>

extern const uint8_t interrupt_breakpoint_after[];

struct breakpoint_observation {
    bool observed;
    bool frame_valid;
};

struct ist_observation {
    bool observed;
    bool frame_on_ist;
    bool interrupted_stack_outside_ist;
};

static struct breakpoint_observation breakpoint_observation;
static struct ist_observation ist_observation;

static void breakpoint_handler(struct interrupt_frame *frame, void *context)
{
    struct breakpoint_observation *observation =
        (struct breakpoint_observation *)context;

    observation->observed = true;
    observation->frame_valid = frame != NULL &&
        frame->vector == 3U &&
        frame->error_code == 0U &&
        frame->rip == (uintptr_t)(const void *)interrupt_breakpoint_after &&
        (frame->rflags & UINT64_C(0x400)) != 0U &&
        (frame->cs & UINT64_C(3)) == 0U &&
        !interrupt_frame_has_stack_tail(frame) &&
        interrupt_frame_stack_selector(frame) == CPU_GDT_DATA_SELECTOR;
}

static void ist_handler(struct interrupt_frame *frame, void *context)
{
    struct ist_observation *observation = (struct ist_observation *)context;
    uintptr_t frame_address = (uintptr_t)(void *)frame;

    observation->observed = true;
    observation->frame_on_ist = cpu_address_on_ist(
        CPU_IST_DOUBLE_FAULT,
        frame_address
    );
    observation->interrupted_stack_outside_ist = frame != NULL &&
        frame->vector == INTERRUPT_IST_TEST_VECTOR &&
        frame->error_code == 0U &&
        interrupt_frame_has_stack_tail(frame) &&
        !cpu_address_on_ist(CPU_IST_DOUBLE_FAULT,
            interrupt_frame_stack_pointer(frame));
}

bool interrupt_frame_layout_self_test(void)
{
    return sizeof(struct interrupt_frame) == 168U &&
        offsetof(struct interrupt_frame, cr2) == 0U &&
        offsetof(struct interrupt_frame, rax) == 120U &&
        offsetof(struct interrupt_frame, vector) == 128U &&
        offsetof(struct interrupt_frame, error_code) == 136U &&
        offsetof(struct interrupt_frame, rip) == 144U &&
        sizeof(struct interrupt_stack_tail) == 16U &&
        cpu_read_cs() == CPU_GDT_CODE_SELECTOR &&
        cpu_read_task_register() == CPU_GDT_TSS_SELECTOR &&
        interrupts_validate() == INTERRUPT_STATUS_OK;
}

bool interrupt_breakpoint_self_test(void)
{
    enum interrupt_status status;
    bool registers_preserved;

    if (!interrupts_ready() || cpu_interrupts_enabled()) {
        return false;
    }

    breakpoint_observation.observed = false;
    breakpoint_observation.frame_valid = false;

    if (interrupt_register_handler(3U, NULL, NULL) !=
        INTERRUPT_STATUS_NULL_HANDLER) {
        return false;
    }

    status = interrupt_register_handler(
        3U,
        breakpoint_handler,
        &breakpoint_observation
    );

    if (status != INTERRUPT_STATUS_OK) {
        return false;
    }

    if (interrupt_register_handler(3U, breakpoint_handler, NULL) !=
        INTERRUPT_STATUS_HANDLER_PRESENT) {
        return false;
    }

    registers_preserved = interrupt_trigger_register_probe();
    status = interrupt_unregister_handler(3U);

    if (status != INTERRUPT_STATUS_OK ||
        interrupt_unregister_handler(3U) != INTERRUPT_STATUS_HANDLER_MISSING) {
        return false;
    }

    return registers_preserved &&
        breakpoint_observation.observed &&
        breakpoint_observation.frame_valid &&
        cpu_stack_canaries_valid();
}

bool interrupt_ist_self_test(void)
{
    enum interrupt_status status;

    if (!interrupts_ready() || cpu_interrupts_enabled()) {
        return false;
    }

    ist_observation.observed = false;
    ist_observation.frame_on_ist = false;
    ist_observation.interrupted_stack_outside_ist = false;

    status = interrupt_register_handler(
        INTERRUPT_IST_TEST_VECTOR,
        ist_handler,
        &ist_observation
    );

    if (status != INTERRUPT_STATUS_OK) {
        return false;
    }

    interrupt_trigger_ist_probe();
    status = interrupt_unregister_handler(INTERRUPT_IST_TEST_VECTOR);

    return status == INTERRUPT_STATUS_OK &&
        ist_observation.observed &&
        ist_observation.frame_on_ist &&
        ist_observation.interrupted_stack_outside_ist &&
        cpu_stack_canaries_valid();
}

bool interrupt_pic_spurious_self_test(void)
{
    uint16_t mask_before;

    if (!interrupts_ready() || !pic_is_initialized() ||
        cpu_interrupts_enabled()) {
        return false;
    }

    mask_before = pic_mask_snapshot();
    interrupt_trigger_spurious_irq7();
    interrupt_trigger_spurious_irq15();
    return pic_mask_snapshot() == mask_before && cpu_stack_canaries_valid();
}
