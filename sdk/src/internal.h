/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_RUNTIME_INTERNAL_H
#define PHIPIA_RUNTIME_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <phipia/runtime.h>

struct phipia_runtime_path {
    uint16_t volume;
    const char *text;
    size_t length;
};

int phipia_runtime_path(const char *input, struct phipia_runtime_path *result);
void phipia_runtime_lock(volatile uint32_t *lock);
void phipia_runtime_unlock(volatile uint32_t *lock);
size_t phipia_allocation_size(const void *pointer);

#endif
