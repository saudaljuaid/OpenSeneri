/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PIC_H
#define PHIPIA_PIC_H

#include <stdbool.h>
#include <stdint.h>

#define PIC_IRQ_COUNT 16U

enum pic_status {
    PIC_STATUS_OK = 0,
    PIC_STATUS_ALREADY_INITIALIZED,
    PIC_STATUS_NOT_INITIALIZED,
    PIC_STATUS_BAD_IRQ,
    PIC_STATUS_CASCADE_IRQ,
    PIC_STATUS_INTERRUPTS_ENABLED,
    PIC_STATUS_RETIRED,
    PIC_STATUS_READBACK_MISMATCH
};

void pic_mask_all_early(void);
enum pic_status pic_initialize(void);
enum pic_status pic_set_mask(uint8_t irq, bool masked);
bool pic_irq_is_real(uint8_t irq);
void pic_send_eoi(uint8_t irq);
enum pic_status pic_retire(void);
bool pic_is_retired(void);
bool pic_is_initialized(void);
uint16_t pic_mask_snapshot(void);
const char *pic_status_string(enum pic_status status);

#endif
