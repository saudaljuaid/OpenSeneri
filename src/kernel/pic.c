/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/pic.h>

#define PIC_MASTER_COMMAND UINT16_C(0x20)
#define PIC_MASTER_DATA UINT16_C(0x21)
#define PIC_SLAVE_COMMAND UINT16_C(0xA0)
#define PIC_SLAVE_DATA UINT16_C(0xA1)
#define PIC_ICW1_INITIALIZE UINT8_C(0x10)
#define PIC_ICW1_ICW4 UINT8_C(0x01)
#define PIC_ICW4_8086 UINT8_C(0x01)
#define PIC_MASTER_VECTOR_OFFSET UINT8_C(0x20)
#define PIC_SLAVE_VECTOR_OFFSET UINT8_C(0x28)
#define PIC_MASTER_HAS_SLAVE_IRQ2 UINT8_C(0x04)
#define PIC_SLAVE_CASCADE_ID UINT8_C(0x02)
#define PIC_OCW2_EOI UINT8_C(0x20)
#define PIC_OCW3_READ_ISR UINT8_C(0x0B)
#define PIC_CASCADE_IRQ 2U

static uint8_t master_mask = UINT8_MAX;
static uint8_t slave_mask = UINT8_MAX;
static bool initialized;
static bool retired;

static void write_masks(void)
{
    cpu_out8(PIC_MASTER_DATA, master_mask);
    cpu_out8(PIC_SLAVE_DATA, slave_mask);
}

static uint8_t read_isr(uint16_t command_port)
{
    cpu_out8(command_port, PIC_OCW3_READ_ISR);
    return cpu_in8(command_port);
}

void pic_mask_all_early(void)
{
    master_mask = UINT8_MAX;
    slave_mask = UINT8_MAX;
    write_masks();
}

enum pic_status pic_initialize(void)
{
    if (initialized) {
        return PIC_STATUS_ALREADY_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return PIC_STATUS_INTERRUPTS_ENABLED;
    }

    pic_mask_all_early();

    cpu_out8(PIC_MASTER_COMMAND, PIC_ICW1_INITIALIZE | PIC_ICW1_ICW4);
    cpu_io_wait();
    cpu_out8(PIC_SLAVE_COMMAND, PIC_ICW1_INITIALIZE | PIC_ICW1_ICW4);
    cpu_io_wait();

    cpu_out8(PIC_MASTER_DATA, PIC_MASTER_VECTOR_OFFSET);
    cpu_io_wait();
    cpu_out8(PIC_SLAVE_DATA, PIC_SLAVE_VECTOR_OFFSET);
    cpu_io_wait();

    cpu_out8(PIC_MASTER_DATA, PIC_MASTER_HAS_SLAVE_IRQ2);
    cpu_io_wait();
    cpu_out8(PIC_SLAVE_DATA, PIC_SLAVE_CASCADE_ID);
    cpu_io_wait();

    cpu_out8(PIC_MASTER_DATA, PIC_ICW4_8086);
    cpu_io_wait();
    cpu_out8(PIC_SLAVE_DATA, PIC_ICW4_8086);
    cpu_io_wait();

    master_mask = UINT8_MAX;
    slave_mask = UINT8_MAX;
    write_masks();
    initialized = true;
    return PIC_STATUS_OK;
}

enum pic_status pic_set_mask(uint8_t irq, bool masked)
{
    uint8_t bit;

    if (!initialized) {
        return PIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return PIC_STATUS_INTERRUPTS_ENABLED;
    }

    /*
     * Retirement is one way. Refusing every later mask change is what makes
     * "the 8259 pair delivers nothing" a property rather than a hope.
     */
    if (retired) {
        return PIC_STATUS_RETIRED;
    }

    if (irq >= PIC_IRQ_COUNT) {
        return PIC_STATUS_BAD_IRQ;
    }

    if (irq == PIC_CASCADE_IRQ) {
        return PIC_STATUS_CASCADE_IRQ;
    }

    if (irq < 8U) {
        bit = (uint8_t)(UINT8_C(1) << irq);

        if (masked) {
            master_mask |= bit;
        } else {
            master_mask &= (uint8_t)~bit;
        }
    } else {
        bit = (uint8_t)(UINT8_C(1) << (irq - 8U));

        if (masked) {
            slave_mask |= bit;
        } else {
            slave_mask &= (uint8_t)~bit;
        }

        if (!masked) {
            master_mask &= (uint8_t)~(UINT8_C(1) << PIC_CASCADE_IRQ);
        } else if (slave_mask == UINT8_MAX) {
            master_mask |= (uint8_t)(UINT8_C(1) << PIC_CASCADE_IRQ);
        }
    }

    write_masks();
    return PIC_STATUS_OK;
}

bool pic_irq_is_real(uint8_t irq)
{
    if (!initialized || irq >= PIC_IRQ_COUNT) {
        return false;
    }

    if (irq == 7U) {
        return (read_isr(PIC_MASTER_COMMAND) & UINT8_C(0x80)) != 0U;
    }

    if (irq == 15U) {
        if ((read_isr(PIC_SLAVE_COMMAND) & UINT8_C(0x80)) == 0U) {
            cpu_out8(PIC_MASTER_COMMAND, PIC_OCW2_EOI);
            return false;
        }
    }

    return true;
}

void pic_send_eoi(uint8_t irq)
{
    if (!initialized || irq >= PIC_IRQ_COUNT) {
        return;
    }

    if (irq >= 8U) {
        cpu_out8(PIC_SLAVE_COMMAND, PIC_OCW2_EOI);
    }

    cpu_out8(PIC_MASTER_COMMAND, PIC_OCW2_EOI);
}

/*
 * Mask every line and refuse to unmask one again. The masks are read back from
 * the interrupt mask registers rather than from Phipia's shadow copy, because
 * the point of retirement is what the hardware holds, not what Phipia believes.
 */
enum pic_status pic_retire(void)
{
    if (!initialized) {
        return PIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return PIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (retired) {
        return PIC_STATUS_RETIRED;
    }

    master_mask = UINT8_MAX;
    slave_mask = UINT8_MAX;
    write_masks();

    if (cpu_in8(PIC_MASTER_DATA) != UINT8_MAX ||
        cpu_in8(PIC_SLAVE_DATA) != UINT8_MAX) {
        return PIC_STATUS_READBACK_MISMATCH;
    }

    retired = true;
    return PIC_STATUS_OK;
}

bool pic_is_retired(void)
{
    return retired;
}

bool pic_is_initialized(void)
{
    return initialized;
}

uint16_t pic_mask_snapshot(void)
{
    return (uint16_t)master_mask | ((uint16_t)slave_mask << 8U);
}

const char *pic_status_string(enum pic_status status)
{
    switch (status) {
    case PIC_STATUS_OK:
        return "ok";
    case PIC_STATUS_ALREADY_INITIALIZED:
        return "PIC was initialized twice";
    case PIC_STATUS_NOT_INITIALIZED:
        return "PIC is not initialized";
    case PIC_STATUS_BAD_IRQ:
        return "PIC IRQ is outside 0-15";
    case PIC_STATUS_CASCADE_IRQ:
        return "PIC cascade IRQ mask is derived from slave masks";
    case PIC_STATUS_INTERRUPTS_ENABLED:
        return "PIC mutation requires interrupts disabled";
    case PIC_STATUS_RETIRED:
        return "PIC is retired and cannot deliver interrupts again";
    case PIC_STATUS_READBACK_MISMATCH:
        return "PIC did not read back a fully masked state";
    default:
        return "unknown PIC status";
    }
}
