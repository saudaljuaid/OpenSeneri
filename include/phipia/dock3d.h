#ifndef PHIPIA_DOCK3D_H
#define PHIPIA_DOCK3D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DOCK3D_ITEM_COUNT 8U
#define DOCK3D_ONE 65536

/* Native fixed-point port of saudaljuaid/3d-dock.  The upstream project uses
 * Cairo doubles; Phipia keeps the same raised-cosine layout and time constants
 * in Q16.16 so the freestanding kernel never enables floating-point state. */
struct dock3d_item_state {
    int32_t scale;
    int32_t target;
    int32_t center_x;
    int32_t advance;
    int32_t press;
    int16_t bounce_frame;
};

struct dock3d_state {
    struct dock3d_item_state items[DOCK3D_ITEM_COUNT];
    int32_t icon;
    int32_t gap;
    int32_t magnification;
    int32_t range;
    int32_t padding;
    int32_t hover;
    int32_t hover_target;
    int32_t mouse_x;
    int32_t raw_mouse_x;
    int32_t raw_mouse_y;
    bool pointer_available;
    bool pointer_in;
    int32_t center_x;
    int32_t panel_x;
    int32_t panel_y;
    int32_t panel_width;
    int32_t panel_height;
    int32_t baseline;
    int32_t top;
    int hot;
    int tooltip_item;
    int32_t tooltip;
    int32_t tooltip_target;
    uint32_t surface_width;
    uint32_t surface_height;
};

void dock3d_initialize(
    struct dock3d_state *dock,
    uint32_t icon_size,
    uint32_t surface_width,
    uint32_t surface_height
);

void dock3d_set_pointer(
    struct dock3d_state *dock,
    int32_t x,
    int32_t y,
    bool available,
    bool magnify
);

void dock3d_advance(
    struct dock3d_state *dock,
    uint32_t frames,
    bool magnify
);

int dock3d_hit(const struct dock3d_state *dock, int32_t x, int32_t y);
void dock3d_launch(struct dock3d_state *dock, size_t index);
bool dock3d_animating(const struct dock3d_state *dock);
bool dock3d_self_test(void);
const char *dock3d_self_test_failure(void);

uint32_t dock3d_round_pixel(int32_t value);
int32_t dock3d_bounce_offset(
    const struct dock3d_state *dock,
    size_t index
);

#endif
