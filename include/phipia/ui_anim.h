/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_UI_ANIM_H
#define PHIPIA_UI_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/ui.h>

/*
 * Warp a rendered panel between its final frame and a dock icon. Each row has
 * an eased width and centre, producing the opening and closing pinch.
 * Geometry uses 16.16 fixed point with 64-bit multiplication. Progress comes
 * from the monotonic clock, so duration is independent of frame rate.
 */

#define UI_ANIM_ONE UINT32_C(65536)
#define UI_ANIM_DEFAULT_OPEN_NS UINT64_C(420000000)
#define UI_ANIM_DEFAULT_CLOSE_NS UINT64_C(320000000)
/* Frames are asked for at this spacing; the clock, not this, sets the pace. */
#define UI_ANIM_FRAME_NS UINT64_C(16000000)
/* Beyond this the warp would need a wider row scratch than it allocates. */
#define UI_ANIM_MAX_WIDTH UINT32_C(4096)

enum ui_anim_status {
    UI_ANIM_STATUS_OK = 0,
    UI_ANIM_STATUS_NULL_ARGUMENT,
    UI_ANIM_STATUS_BAD_GEOMETRY,
    UI_ANIM_STATUS_NO_MEMORY,
    UI_ANIM_STATUS_NOT_RUNNING,
    UI_ANIM_STATUS_SURFACE_FAILURE
};

struct ui_anim {
    bool running;
    bool opening;
    uint64_t start_ns;
    uint64_t duration_ns;
    struct ui_rect origin;    /* the dock icon the window comes from */
    struct ui_rect frame;     /* where the window rests when it is open */
    uint32_t *pixels;         /* frame.width * frame.height, then one row */
    uint32_t *row;
    uint32_t progress;        /* 0 .. UI_ANIM_ONE, last advanced value */
};

void ui_anim_reset(struct ui_anim *anim);

/*
 * Copy the drawn panel out of the canvas and start the clock. The caller
 * must have drawn the panel at `frame` already: this reads pixels, it does
 * not know how to make them.
 */
enum ui_anim_status ui_anim_begin(
    struct ui_anim *anim,
    const struct surface *source,
    struct ui_rect frame,
    struct ui_rect origin,
    bool opening,
    uint64_t now_ns,
    uint64_t duration_ns
);

void ui_anim_end(struct ui_anim *anim);
bool ui_anim_running(const struct ui_anim *anim);

/* Recompute progress from the clock; clears `running` once it is spent. */
uint32_t ui_anim_advance(struct ui_anim *anim, uint64_t now_ns);

/* Everything the warp can touch, for the damage rectangle. */
struct ui_rect ui_anim_bounds(const struct ui_anim *anim);

enum ui_anim_status ui_anim_draw(
    struct ui_anim *anim,
    struct surface *target,
    struct ui_rect clip
);

/* Smootherstep: zero first and second derivative at both ends. */
uint32_t ui_anim_ease(uint32_t t);

bool ui_anim_self_test(void);
const char *ui_anim_self_test_failure(void);
const char *ui_anim_status_string(enum ui_anim_status status);

#endif
