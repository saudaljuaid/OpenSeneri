/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_WINDOW_H
#define PHIPIA_WINDOW_H

#include <stddef.h>
#include <stdint.h>
#include <phipia/abi.h>

int phipia_window_create(const char *title, uint32_t width, uint32_t height,
    struct phipia_window_create_response *response);
long phipia_surface_present(phipia_handle_t window,
    const struct phipia_rect *rectangles, size_t count);
long phipia_event_read(phipia_handle_t events, struct phipia_event *event);
long phipia_event_wait(phipia_handle_t events, uint64_t deadline_ns);
long phipia_pointer_capture(phipia_handle_t window, int capture);

#endif
