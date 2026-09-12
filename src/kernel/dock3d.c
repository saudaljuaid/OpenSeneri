/*
 * Phipia native port of the model and animation engine from:
 *   https://github.com/saudaljuaid/3d-dock (commit 8ab14d0)
 *
 * The upstream source is MIT licensed.  Its Cairo/X11 renderer and libm
 * doubles cannot execute in Phipia's freestanding kernel, so this file keeps
 * the same raised-cosine curve, resting-grid measurement, eased-width layout,
 * pointer anchoring, 1.95x magnification, time constants, press squash and
 * decaying launch bounce in deterministic Q16.16 fixed point.
 */
#include <phipia/dock3d.h>

#include <limits.h>

#define DOCK3D_SCALE_RATE 17133
#define DOCK3D_HOVER_RATE 11079
#define DOCK3D_MOUSE_RATE 34813
#define DOCK3D_TOOLTIP_RATE 13885
#define DOCK3D_MAGNIFICATION 127795
#define DOCK3D_BOUNCE_FRAMES 70

static const char *self_test_failure = "3d-dock native port self-test passed";

static const int32_t cosine_kernel[65U] = {
    65536, 65497, 65378, 65181, 64906, 64554, 64125, 63621,
    63042, 62390, 61667, 60874, 60014, 59088, 58098, 57047,
    55938, 54774, 53556, 52288, 50973, 49614, 48215, 46778,
    45308, 43807, 42280, 40730, 39161, 37576, 35980, 34376,
    32768, 31160, 29556, 27960, 26375, 24806, 23256, 21729,
    20228, 18758, 17321, 15922, 14563, 13248, 11980, 10762,
    9598, 8489, 7438, 6448, 5522, 4662, 3869, 3146,
    2494, 1915, 1411, 982, 630, 355, 158, 39, 0
};

static const int32_t bounce_curve[DOCK3D_BOUNCE_FRAMES] = {
    0, -8105, -15354, -21676, -27028, -31386, -34750, -37139,
    -38589, -39153, -38896, -37893, -36228, -33992, -31276,
    -28176, -24787, -21199, -17502, -13776, -10099, -6539,
    -3155, 0, -2884, -5463, -7713, -9617, -11168, -12365,
    -13215, -13731, -13931, -13840, -13483, -12891, -12095,
    -11129, -10026, -8820, -7543, -6227, -4902, -3593, -2327,
    -1123, 0, -1026, -1944, -2744, -3422, -3974, -4400,
    -4702, -4886, -4957, -4925, -4798, -4587, -4304, -3960,
    -3567, -3138, -2684, -2216, -1744, -1279, -828, -399, 0
};

static int32_t fixed_multiply(int32_t left, int32_t right)
{
    return (int32_t)(((int64_t)left * (int64_t)right) / DOCK3D_ONE);
}

static int32_t fixed_approach(int32_t value, int32_t target, int32_t rate)
{
    return value + fixed_multiply(target - value, rate);
}

static int32_t absolute_fixed(int32_t value)
{
    return value < 0 ? -value : value;
}

static int32_t raised_cosine(int32_t distance, int32_t range)
{
    if (range <= 0 || distance >= range) {
        return 0;
    }
    const int64_t scaled = (int64_t)distance * 64;
    const size_t index = (size_t)(scaled / range);
    const int32_t fraction = (int32_t)(((scaled % range) * DOCK3D_ONE) /
        range);
    const int32_t first = cosine_kernel[index];
    const int32_t second = cosine_kernel[index + 1U];

    return first + fixed_multiply(second - first, fraction);
}

static int32_t item_advance(
    const struct dock3d_state *dock,
    const struct dock3d_item_state *item
)
{
    const int32_t gap_factor = 26214 + fixed_multiply(39322, item->scale);

    return fixed_multiply(dock->icon, item->scale) +
        fixed_multiply(dock->gap, gap_factor);
}

static void layout_dock(struct dock3d_state *dock, bool magnify)
{
    int32_t resting_at[DOCK3D_ITEM_COUNT];
    int32_t magnified_at[DOCK3D_ITEM_COUNT];
    const int32_t screen_center =
        (int32_t)(dock->surface_width * DOCK3D_ONE / 2U);
    const int32_t resting_cell = dock->icon + dock->gap;
    const int32_t resting_width = resting_cell * (int32_t)DOCK3D_ITEM_COUNT;
    const int32_t resting_x = screen_center - resting_width / 2;
    const int32_t resting_end = resting_x + resting_width;
    int32_t row_width = 0;
    int32_t x = resting_x;

    dock->panel_height = fixed_multiply(dock->icon, 39322);
    dock->baseline = (int32_t)(dock->surface_height * DOCK3D_ONE) -
        dock->panel_height;
    dock->panel_y = dock->baseline;

    const int32_t half = resting_width / 2 + dock->padding;
    const int32_t top = dock->baseline - dock->icon;
    dock->pointer_in = dock->pointer_available &&
        dock->raw_mouse_x > screen_center - half - 2 * DOCK3D_ONE &&
        dock->raw_mouse_x < screen_center + half + 2 * DOCK3D_ONE &&
        dock->raw_mouse_y > top &&
        dock->raw_mouse_y < (int32_t)(dock->surface_height * DOCK3D_ONE);
    dock->hover_target = dock->pointer_in && magnify ? DOCK3D_ONE : 0;

    for (size_t index = 0U; index < DOCK3D_ITEM_COUNT; ++index) {
        const int32_t resting_center = x + resting_cell / 2;
        const int32_t kernel = raised_cosine(
            absolute_fixed(dock->mouse_x - resting_center), dock->range);
        const int32_t strength = fixed_multiply(kernel, dock->hover);

        resting_at[index] = x;
        dock->items[index].target = DOCK3D_ONE + fixed_multiply(
            dock->magnification - DOCK3D_ONE, strength);
        dock->items[index].advance = item_advance(dock,
            &dock->items[index]);
        row_width += dock->items[index].advance;
        x += resting_cell;
    }

    int32_t row_x = screen_center - row_width / 2;
    x = row_x;
    for (size_t index = 0U; index < DOCK3D_ITEM_COUNT; ++index) {
        magnified_at[index] = x;
        x += dock->items[index].advance;
    }

    int32_t mapped;
    if (dock->mouse_x <= resting_x) {
        mapped = row_x - (resting_x - dock->mouse_x);
    } else if (dock->mouse_x >= resting_end) {
        mapped = row_x + row_width + (dock->mouse_x - resting_end);
    } else {
        mapped = row_x + row_width;
        for (size_t index = 0U; index < DOCK3D_ITEM_COUNT; ++index) {
            if (dock->mouse_x < resting_at[index] + resting_cell) {
                const int32_t fraction = (int32_t)(
                    ((int64_t)(dock->mouse_x - resting_at[index]) *
                        DOCK3D_ONE) / resting_cell);
                mapped = magnified_at[index] + fixed_multiply(
                    fraction, dock->items[index].advance);
                break;
            }
        }
    }
    int32_t delta = dock->mouse_x - mapped;
    dock->panel_width = row_width + dock->padding * 2;
    dock->panel_x = row_x + delta - dock->padding;
    const int32_t surface_width =
        (int32_t)(dock->surface_width * DOCK3D_ONE);
    if (dock->panel_width + 8 * DOCK3D_ONE < surface_width) {
        if (dock->panel_x < 4 * DOCK3D_ONE) {
            delta += 4 * DOCK3D_ONE - dock->panel_x;
        } else if (dock->panel_x + dock->panel_width >
                surface_width - 4 * DOCK3D_ONE) {
            delta -= dock->panel_x + dock->panel_width -
                (surface_width - 4 * DOCK3D_ONE);
        }
        dock->panel_x = row_x + delta - dock->padding;
    }

    dock->center_x = dock->panel_x + dock->panel_width / 2;
    dock->top = dock->baseline;
    for (size_t index = 0U; index < DOCK3D_ITEM_COUNT; ++index) {
        struct dock3d_item_state *item = &dock->items[index];
        item->center_x = magnified_at[index] + item->advance / 2 + delta;
        const int32_t item_top = dock->baseline -
            fixed_multiply(dock->icon, item->scale) +
            dock3d_bounce_offset(dock, index);
        if (item_top < dock->top) {
            dock->top = item_top;
        }
    }
    dock->hot = dock->pointer_available ? dock3d_hit(dock,
        dock->raw_mouse_x / DOCK3D_ONE,
        dock->raw_mouse_y / DOCK3D_ONE) : -1;
    if (dock->hot >= 0) {
        dock->tooltip_item = dock->hot;
    }
    dock->tooltip_target = dock->hot >= 0 ? DOCK3D_ONE : 0;
}

void dock3d_initialize(
    struct dock3d_state *dock,
    uint32_t icon_size,
    uint32_t surface_width,
    uint32_t surface_height
)
{
    if (dock == NULL) {
        return;
    }
    *dock = (struct dock3d_state){ 0 };
    dock->icon = (int32_t)(icon_size * DOCK3D_ONE);
    dock->gap = fixed_multiply(dock->icon, 10486);
    dock->magnification = DOCK3D_MAGNIFICATION;
    dock->range = fixed_multiply(dock->icon, 180224);
    dock->padding = fixed_multiply(dock->icon, 18350);
    dock->surface_width = surface_width;
    dock->surface_height = surface_height;
    dock->center_x = (int32_t)(surface_width * DOCK3D_ONE / 2U);
    dock->raw_mouse_x = dock->center_x;
    dock->raw_mouse_y = (int32_t)(surface_height * DOCK3D_ONE);
    dock->mouse_x = dock->raw_mouse_x;
    dock->hot = -1;
    dock->tooltip_item = -1;
    for (size_t index = 0U; index < DOCK3D_ITEM_COUNT; ++index) {
        dock->items[index].scale = DOCK3D_ONE;
        dock->items[index].target = DOCK3D_ONE;
        dock->items[index].bounce_frame = -1;
    }
    layout_dock(dock, true);
}

void dock3d_set_pointer(
    struct dock3d_state *dock,
    int32_t x,
    int32_t y,
    bool available,
    bool magnify
)
{
    if (dock == NULL) {
        return;
    }
    dock->raw_mouse_x = x * DOCK3D_ONE;
    dock->raw_mouse_y = y * DOCK3D_ONE;
    dock->pointer_available = available;
    if (!dock->pointer_in) {
        dock->mouse_x = dock->raw_mouse_x;
    }
    layout_dock(dock, magnify);
}

void dock3d_advance(
    struct dock3d_state *dock,
    uint32_t frames,
    bool magnify
)
{
    if (dock == NULL) {
        return;
    }
    if (frames > 8U) {
        frames = 8U;
    }
    for (uint32_t frame = 0U; frame < frames; ++frame) {
        layout_dock(dock, magnify);
        dock->hover = fixed_approach(dock->hover,
            dock->hover_target, DOCK3D_HOVER_RATE);
        if (dock->pointer_in) {
            dock->mouse_x = fixed_approach(dock->mouse_x,
                dock->raw_mouse_x, DOCK3D_MOUSE_RATE);
        } else {
            dock->mouse_x = dock->raw_mouse_x;
        }
        for (size_t index = 0U; index < DOCK3D_ITEM_COUNT; ++index) {
            struct dock3d_item_state *item = &dock->items[index];
            item->scale = fixed_approach(item->scale, item->target,
                DOCK3D_SCALE_RATE);
            item->press = fixed_approach(item->press, 0,
                DOCK3D_HOVER_RATE);
            if (item->bounce_frame >= 0) {
                item->bounce_frame += 1;
                if (item->bounce_frame >= DOCK3D_BOUNCE_FRAMES) {
                    item->bounce_frame = -1;
                }
            }
        }
        dock->tooltip = fixed_approach(dock->tooltip,
            dock->tooltip_target, DOCK3D_TOOLTIP_RATE);
    }
    layout_dock(dock, magnify);
    if (dock->tooltip < 262) {
        dock->tooltip = 0;
        if (dock->hot < 0) {
            dock->tooltip_item = -1;
        }
    }
}

int dock3d_hit(const struct dock3d_state *dock, int32_t x, int32_t y)
{
    if (dock == NULL) {
        return -1;
    }
    const int32_t fixed_x = x * DOCK3D_ONE;
    const int32_t fixed_y = y * DOCK3D_ONE;
    const int32_t resting_cell = dock->icon + dock->gap;
    const int32_t resting_width = resting_cell *
        (int32_t)DOCK3D_ITEM_COUNT;
    const int32_t resting_x =
        (int32_t)(dock->surface_width * DOCK3D_ONE / 2U) -
            resting_width / 2;
    const int32_t top = dock->baseline -
        fixed_multiply(dock->icon, dock->magnification);

    /* The artwork eases, magnifies, squashes and bounces; its input lanes do
     * not.  Stable, non-overlapping lanes prevent a visible hovered icon from
     * moving out from under a press between animation frames. */
    if (fixed_x < resting_x || fixed_x >= resting_x + resting_width ||
            fixed_y < top ||
            fixed_y > dock->baseline + dock->panel_height) {
        return -1;
    }
    return (int)((fixed_x - resting_x) / resting_cell);
}

void dock3d_launch(struct dock3d_state *dock, size_t index)
{
    if (dock == NULL || index >= DOCK3D_ITEM_COUNT) {
        return;
    }
    dock->items[index].press = DOCK3D_ONE;
    dock->items[index].bounce_frame = 0;
}

bool dock3d_animating(const struct dock3d_state *dock)
{
    if (dock == NULL) {
        return false;
    }
    if (absolute_fixed(dock->hover - dock->hover_target) > 66 ||
            absolute_fixed(dock->tooltip - dock->tooltip_target) > 197 ||
            (dock->pointer_in &&
                absolute_fixed(dock->mouse_x - dock->raw_mouse_x) > 32)) {
        return true;
    }
    for (size_t index = 0U; index < DOCK3D_ITEM_COUNT; ++index) {
        const struct dock3d_item_state *item = &dock->items[index];
        if (absolute_fixed(item->scale - item->target) > 20 ||
                item->bounce_frame >= 0 || item->press > 262) {
            return true;
        }
    }
    return false;
}

uint32_t dock3d_round_pixel(int32_t value)
{
    if (value <= 0) {
        return 0U;
    }
    return (uint32_t)(((int64_t)value + DOCK3D_ONE / 2) / DOCK3D_ONE);
}

int32_t dock3d_bounce_offset(
    const struct dock3d_state *dock,
    size_t index
)
{
    if (dock == NULL || index >= DOCK3D_ITEM_COUNT ||
            dock->items[index].bounce_frame < 0) {
        return 0;
    }
    return fixed_multiply(dock->icon,
        bounce_curve[(size_t)dock->items[index].bounce_frame]);
}

bool dock3d_self_test(void)
{
    struct dock3d_state dock;

    self_test_failure = "3d-dock native port self-test passed";
    dock3d_initialize(&dock, 58U, 1024U, 768U);
    const int32_t rest_width = dock.panel_width;
    const int32_t target_x = (int32_t)dock3d_round_pixel(
        dock.items[2U].center_x);
    const int32_t target_y = (int32_t)dock3d_round_pixel(
        dock.baseline - dock.icon / 2);

    if (dock.hot != -1 || dock.items[0U].scale != DOCK3D_ONE ||
            dock.panel_x < 0 ||
            dock.panel_x + dock.panel_width >
                (int32_t)(1024U * DOCK3D_ONE)) {
        self_test_failure = "3d-dock rest geometry is invalid";
        return false;
    }
    dock3d_set_pointer(&dock, target_x, target_y, true, true);
    for (size_t step = 0U; step < 10U; ++step) {
        dock3d_advance(&dock, 8U, true);
    }
    if (dock.hot != 2 || dock.items[2U].scale < 124000 ||
            dock.panel_width <= rest_width) {
        self_test_failure = "3d-dock raised-cosine hover did not magnify";
        return false;
    }
    for (size_t index = 1U; index < DOCK3D_ITEM_COUNT; ++index) {
        if (dock.items[index - 1U].center_x >= dock.items[index].center_x) {
            self_test_failure = "3d-dock eased widths overlap or reverse";
            return false;
        }
    }
    dock3d_launch(&dock, 2U);
    dock3d_advance(&dock, 2U, true);
    if (dock.items[2U].press <= 0 ||
            dock3d_bounce_offset(&dock, 2U) >= 0) {
        self_test_failure = "3d-dock launch press or bounce is invalid";
        return false;
    }
    dock3d_set_pointer(&dock, 20, 20, true, false);
    for (size_t step = 0U; step < 24U; ++step) {
        dock3d_advance(&dock, 8U, false);
    }
    for (size_t index = 0U; index < DOCK3D_ITEM_COUNT; ++index) {
        const int32_t difference = dock.items[index].scale - DOCK3D_ONE;
        if (difference < -32 || difference > 32) {
            self_test_failure = "3d-dock magnification did not settle at rest";
            return false;
        }
    }
    return true;
}

const char *dock3d_self_test_failure(void)
{
    return self_test_failure;
}
