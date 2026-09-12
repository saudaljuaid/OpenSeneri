/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ABI_WINDOW_H
#define PHIPIA_ABI_WINDOW_H

#include <phipia/abi/base.h>

#define PHIPIA_WINDOW_TITLE_MAX 31U
#define PHIPIA_DAMAGE_MAX 8U
#define PHIPIA_PIXEL_XRGB8888 UINT32_C(1)

struct phipia_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct phipia_window_create_request {
    uint32_t size;
    uint32_t version;
    uint64_t title;
    uint32_t title_length;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct phipia_window_create_response {
    uint32_t size;
    uint32_t version;
    phipia_handle_t window;
    phipia_handle_t events;
    uint64_t surface_address;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t pixel_format;
} __attribute__((packed));

struct phipia_present_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t window;
    uint64_t rectangles;
    uint32_t rectangle_count;
    uint32_t flags;
} __attribute__((packed));

enum phipia_event_type {
    PHIPIA_EVENT_NONE = 0,
    PHIPIA_EVENT_KEY = 1,
    PHIPIA_EVENT_POINTER_MOVE = 2,
    PHIPIA_EVENT_POINTER_BUTTON = 3,
    PHIPIA_EVENT_FOCUS = 4,
    PHIPIA_EVENT_CLOSE = 5,
    PHIPIA_EVENT_QUEUE_OVERFLOW = 6
};

enum phipia_key_action {
    PHIPIA_KEY_RELEASED = 0,
    PHIPIA_KEY_PRESSED = 1,
    PHIPIA_KEY_REPEATED = 2
};

struct phipia_event {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    uint32_t flags;
    uint64_t monotonic_ns;
    int32_t x;
    int32_t y;
    int32_t delta_x;
    int32_t delta_y;
    uint32_t code;
    uint32_t value;
    uint32_t modifiers;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct phipia_rect) == 16U,
    "Phipia rectangle ABI changed");
_Static_assert(sizeof(struct phipia_window_create_request) == 40U,
    "Phipia window-create request ABI changed");
_Static_assert(sizeof(struct phipia_window_create_response) == 48U,
    "Phipia window-create response ABI changed");
_Static_assert(sizeof(struct phipia_present_request) == 32U,
    "Phipia present ABI changed");
_Static_assert(sizeof(struct phipia_event) == 56U,
    "Phipia event ABI changed");

#endif
