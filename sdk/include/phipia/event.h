/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_EVENT_H
#define PHIPIA_EVENT_H

#include <stddef.h>
#include <stdint.h>

#include <phipia/abi.h>

long phipia_wait(struct phipia_wait_item *items, size_t count,
    uint64_t deadline_ns);
long phipia_timer_create(void);
long phipia_timer_set(phipia_handle_t timer, uint64_t deadline_ns);
long phipia_cancel(phipia_handle_t handle);

#endif
