/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NATIVE_FPU_H
#define PHIPIA_NATIVE_FPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_FPU_STATE_BYTES 512U
#define NATIVE_FPU_STATE_ALIGNMENT 16U

struct native_fpu_state {
    uint8_t bytes[NATIVE_FPU_STATE_BYTES];
} __attribute__((aligned(NATIVE_FPU_STATE_ALIGNMENT)));

bool native_fpu_initialize(void);
bool native_fpu_state_initialize(struct native_fpu_state *state);
bool native_fpu_save(struct native_fpu_state *state);
bool native_fpu_restore(const struct native_fpu_state *state);
bool native_fpu_is_ready(void);
bool native_fpu_capability_self_test(size_t *completed_tests);
bool native_fpu_self_test(size_t *completed_tests);

void native_fxsave(void *state);
void native_fxrstor(const void *state);
void native_fxinit(void *state);

#endif
