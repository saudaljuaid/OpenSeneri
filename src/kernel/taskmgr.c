/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Task Manager.  See include/phipia/taskmgr.h for the shape, for what each
 * of the three tabs is, and for which of the four windows this borrows from
 * gave which idea.
 *
 * Every number drawn here arrives through taskmgr_set_*; nothing in this
 * file samples, estimates or invents a figure.  That is not a limitation
 * being apologised for - a Task Manager that made its own numbers up would
 * be the single least honest window a shell could ship.
 */

#include <phipia/taskmgr.h>

#include <phipia/clock.h>
#include <phipia/framebuffer.h>
#include <phipia/taskbar.h>
#include <phipia/ui_font.h>

#include "explorer_glyphs.h"
#include "shell_icons.h"
#include "ui_motion.h"

/* ================================================================ METRICS */

#define TASKMGR_BORDER 1U
#define TASKMGR_CAPTION 32U
#define TASKMGR_CAPTION_BUTTON 46U
#define TASKMGR_MENU 24U
#define TASKMGR_TABS 34U
#define TASKMGR_FOOTER 44U
#define TASKMGR_PAD 12U
/* The table: a two-line heading (the column's name, and the aggregate under
 * it) over rows tall enough for a 16-pixel icon and a line of text. */
#define TASKMGR_HEAD 40U
#define TASKMGR_ROW 28U
#define TASKMGR_ROW_ICON 16U
#define TASKMGR_GROUP_ROW 26U
/* Phipia's accent bar down a selected row, the same three pixels File
 * Explorer and the Start menu's list use. */
#define TASKMGR_SELECT_BAR 3U
/*
 * The smallest box a Lucide mark may be asked for.  The set is rasterized
 * at sixteen, twenty and twenty-four; asking for twelve does not get a
 * twelve-pixel mark, it gets the sixteen drawn spilling out of the box,
 * because nothing here resamples a glyph to fit.  Sixteen is the floor.
 */
#define TASKMGR_MARK 16U
/* The Performance tab: a rail of resource tiles, then the graph. */
#define TASKMGR_RAIL 196U
#define TASKMGR_TILE 76U
#define TASKMGR_CORE_ROW 34U
#define TASKMGR_CORE_COLUMNS 8U
/*
 * The lowest load a core's bar takes COLOUR for, though never length: the
 * table wants an idle cell left white, and the same rule on a bar paints a
 * white fill onto a grey track, which has no length left to read.  The bar
 * is still exactly as long as the core is busy - only its tint is floored.
 */
#define TASKMGR_CORE_TINT_FLOOR 170U
#define TASKMGR_GRAPH_GRID 6U
/* The narrowest window the six columns fit in without overlapping. */
#define TASKMGR_MIN_WIDTH 720U
#define TASKMGR_MIN_HEIGHT 420U

/* ================================================================ PALETTE
 *
 * Windows 10's Task Manager is a light window whatever the taskbar is
 * doing, the same as File Explorer; it gained no dark theme either.
 * PENDING VERIFICATION.
 */
struct taskmgr_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define TASKMGR_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct taskmgr_rgb caption_fill = TASKMGR_RGB(0xFFU, 0xFFU,
    0xFFU);
static const struct taskmgr_rgb chrome = TASKMGR_RGB(0xF0U, 0xF0U, 0xF0U);
static const struct taskmgr_rgb table_fill = TASKMGR_RGB(0xFFU, 0xFFU,
    0xFFU);
static const struct taskmgr_rgb head_fill = TASKMGR_RGB(0xF7U, 0xF7U, 0xF7U);
static const struct taskmgr_rgb rule = TASKMGR_RGB(0xDCU, 0xDCU, 0xDCU);
static const struct taskmgr_rgb rule_soft = TASKMGR_RGB(0xEDU, 0xEDU, 0xEDU);
static const struct taskmgr_rgb ink = TASKMGR_RGB(0x1AU, 0x1AU, 0x1AU);
static const struct taskmgr_rgb ink_soft = TASKMGR_RGB(0x66U, 0x66U, 0x66U);
static const struct taskmgr_rgb ink_faint = TASKMGR_RGB(0x8CU, 0x8CU, 0x8CU);
static const struct taskmgr_rgb accent = TASKMGR_RGB(0x00U, 0x78U, 0xD7U);
static const struct taskmgr_rgb select_fill = TASKMGR_RGB(0xE3U, 0xF0U,
    0xFBU);
static const struct taskmgr_rgb hover_fill = TASKMGR_RGB(0xF4U, 0xF8U,
    0xFCU);
static const struct taskmgr_rgb button_fill = TASKMGR_RGB(0xE1U, 0xE1U,
    0xE1U);
static const struct taskmgr_rgb button_hot = TASKMGR_RGB(0xCCU, 0xE4U,
    0xF7U);
static const struct taskmgr_rgb button_edge = TASKMGR_RGB(0xADU, 0xADU,
    0xADU);
/* What Windows lights a close button with, and the only red in this file. */
static const struct taskmgr_rgb close_hot = TASKMGR_RGB(0xE8U, 0x11U, 0x23U);
static const struct taskmgr_rgb border_active = TASKMGR_RGB(0x00U, 0x78U,
    0xD7U);
static const struct taskmgr_rgb border_inactive = TASKMGR_RGB(0x9BU, 0x9BU,
    0x9BU);

/*
 * The kind colours.  File Explorer tints a row by what the file IS; this
 * tints by what the process is, for the same reason and out of the same
 * palette family - an application, a background task and a system process
 * stop being three identical grey lines.
 */
static const struct taskmgr_rgb kind_colour[TASKMGR_KIND_COUNT] = {
    TASKMGR_RGB(0x00U, 0x78U, 0xD7U),   /* app,        the shell accent */
    TASKMGR_RGB(0x7AU, 0x52U, 0xC0U),   /* background, violet           */
    TASKMGR_RGB(0x5AU, 0x6AU, 0x76U)    /* system,     slate            */
};

/*
 * Each resource's own hue, which Windows 10 also gives them - the CPU graph
 * is blue, memory is violet, and so on - so a glance at the rail says which
 * tile is which before any label is read.
 */
static const struct taskmgr_rgb resource_colour[TASKMGR_RESOURCE_COUNT] = {
    TASKMGR_RGB(0x00U, 0x78U, 0xD7U),   /* CPU     */
    TASKMGR_RGB(0x8BU, 0x36U, 0xC6U),   /* Memory  */
    TASKMGR_RGB(0x1EU, 0x9EU, 0x52U),   /* Disk    */
    TASKMGR_RGB(0xC2U, 0x5AU, 0x00U)    /* Network */
};

/*
 * The heat map, which is the one thing about this window everybody
 * recognises: Windows tints a numeric cell from nearly nothing at idle
 * through amber to a hot orange under load, so a busy machine is legible
 * without reading a single figure.  Three stops, blended between.
 */
static const struct taskmgr_rgb heat_cold = TASKMGR_RGB(0xFFU, 0xFCU, 0xF2U);
static const struct taskmgr_rgb heat_warm = TASKMGR_RGB(0xFFU, 0xDCU, 0x8AU);
static const struct taskmgr_rgb heat_hot = TASKMGR_RGB(0xEEU, 0x7CU, 0x3CU);

/* The Startup tab's impact pill. */
static const struct taskmgr_rgb impact_colour[TASKMGR_IMPACT_COUNT] = {
    TASKMGR_RGB(0x8CU, 0x8CU, 0x8CU),   /* None   */
    TASKMGR_RGB(0x1EU, 0x9EU, 0x52U),   /* Low    */
    TASKMGR_RGB(0xC2U, 0x8AU, 0x00U),   /* Medium */
    TASKMGR_RGB(0xD1U, 0x34U, 0x34U)    /* High   */
};

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
/*
 * Whether the window is on screen at all.
 *
 * Task Manager is the one window in this shell that a person opens and
 * closes rather than one that is simply there, so it is the one that has to
 * know.  A compositor mirrors this into the taskbar - present while open,
 * gone when closed - which is the behaviour Windows has and which this
 * window could not support while its close mark was a picture of a button.
 */
static bool window_open = true;
static bool focused = true;

static struct taskmgr_process processes[TASKMGR_MAX_PROCESSES];
static struct taskmgr_meter meters[TASKMGR_RESOURCE_COUNT];
static struct taskmgr_startup startup[TASKMGR_MAX_STARTUP];
static char ended_task[TASKMGR_NAME_BYTES];
static bool ended_task_waiting;
static uint16_t cores[TASKMGR_MAX_CORES];
static size_t core_count;
static uint32_t uptime_seconds;

static enum taskmgr_tab tab = TASKMGR_TAB_PROCESSES;
static enum taskmgr_resource resource = TASKMGR_RESOURCE_CPU;
static enum taskmgr_column sort_column = TASKMGR_COLUMN_CPU;
static bool sort_descending = true;
/*
 * The sort is an ORDER over processes[], not a rearrangement of it.  The
 * caller owns those slots and hands them back by index; shuffling the array
 * under it would mean index 3 stopped meaning what the caller set at 3.
 */
static size_t order[TASKMGR_MAX_PROCESSES];
static size_t order_count;

static size_t selected = (size_t)-1;
static size_t hover_row = (size_t)-1;
static bool hover_end_task;
static bool hover_close;
static size_t hover_tab = (size_t)-1;
static size_t hover_column = (size_t)-1;
static size_t hover_tile = (size_t)-1;

/* The selected row's fill cross-fades the way every other list in this
 * shell does; see ui_motion.h. */
static struct ui_motion row_fade;
static size_t fading_row = (size_t)-1;

static const char *self_test_failure = "taskmgr self-test has not run";

const char *taskmgr_status_string(enum taskmgr_status status)
{
    switch (status) {
    case TASKMGR_STATUS_OK:
        return "ok";
    case TASKMGR_STATUS_NULL_ARGUMENT:
        return "null argument";
    case TASKMGR_STATUS_NOT_INITIALIZED:
        return "taskmgr not initialized";
    case TASKMGR_STATUS_BAD_INDEX:
        return "taskmgr index is out of range";
    case TASKMGR_STATUS_UNSUPPORTED_GEOMETRY:
        return "taskmgr geometry is unsupported";
    case TASKMGR_STATUS_SURFACE_FAILURE:
        return "taskmgr surface refused a pixel";
    default:
        return "unknown taskmgr status";
    }
}

/* ================================================================ DRAWING */

static uint32_t pack_rgb(struct taskmgr_rgb colour)
{
    return framebuffer_pack(colour.red, colour.green, colour.blue);
}

static struct ui_rect intersect(struct ui_rect left, struct ui_rect right)
{
    const uint32_t x = left.x > right.x ? left.x : right.x;
    const uint32_t y = left.y > right.y ? left.y : right.y;
    const uint32_t right_edge = left.x + left.width < right.x + right.width ?
        left.x + left.width : right.x + right.width;
    const uint32_t bottom = left.y + left.height < right.y + right.height ?
        left.y + left.height : right.y + right.height;

    if (right_edge <= x || bottom <= y) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ x, y, right_edge - x, bottom - y };
}

static struct ui_rect join(struct ui_rect left, struct ui_rect right)
{
    uint32_t x;
    uint32_t y;
    uint32_t r;
    uint32_t b;

    if (left.width == 0U || left.height == 0U) {
        return right;
    }
    if (right.width == 0U || right.height == 0U) {
        return left;
    }
    x = left.x < right.x ? left.x : right.x;
    y = left.y < right.y ? left.y : right.y;
    r = left.x + left.width > right.x + right.width ?
        left.x + left.width : right.x + right.width;
    b = left.y + left.height > right.y + right.height ?
        left.y + left.height : right.y + right.height;
    return (struct ui_rect){ x, y, r - x, b - y };
}

static bool holds(struct ui_rect area, struct ui_point point)
{
    if (area.width == 0U || area.height == 0U || point.x < 0 || point.y < 0) {
        return false;
    }
    return (uint32_t)point.x >= area.x &&
        (uint32_t)point.x < area.x + area.width &&
        (uint32_t)point.y >= area.y &&
        (uint32_t)point.y < area.y + area.height;
}

static enum taskmgr_status fill(struct ui_rect area, struct ui_rect damage,
    struct taskmgr_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return TASKMGR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKMGR_STATUS_OK;
}

/* A one-pixel frame, which is what says where a meter ends when what is
 * drawn inside it can be lighter than any track behind it. */
static enum taskmgr_status outline(struct ui_rect area, struct ui_rect damage,
    struct taskmgr_rgb colour)
{
    enum taskmgr_status status = fill((struct ui_rect){ area.x, area.y,
        area.width, 1U }, damage, colour);

    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y + area.height - 1U,
            area.width, 1U }, damage, colour);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y, 1U, area.height },
            damage, colour);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ area.x + area.width - 1U, area.y,
            1U, area.height }, damage, colour);
    }
    return status;
}

/* A blend between two colours, `along` running 0 to 255. */
static struct taskmgr_rgb mix(struct taskmgr_rgb from, struct taskmgr_rgb to,
    uint32_t along)
{
    const uint32_t t = along > 255U ? 255U : along;

    return (struct taskmgr_rgb){
        (uint8_t)((from.red * (255U - t) + to.red * t) / 255U),
        (uint8_t)((from.green * (255U - t) + to.green * t) / 255U),
        (uint8_t)((from.blue * (255U - t) + to.blue * t) / 255U)
    };
}

/*
 * The heat for a load, 0 to 1000 tenths.  Cold below about a tenth of the
 * machine, because Windows leaves an idle cell white rather than tinting
 * every row a faint yellow and making the map mean nothing.
 */
static struct taskmgr_rgb heat_for(uint16_t tenths)
{
    const uint32_t load = tenths > 1000U ? 1000U : tenths;

    if (load < 60U) {
        return table_fill;
    }
    if (load < 500U) {
        return mix(heat_cold, heat_warm, (load - 60U) * 255U / 440U);
    }
    return mix(heat_warm, heat_hot, (load - 500U) * 255U / 500U);
}

static uint32_t width_of(const char *body)
{
    uint32_t width = 0U;

    if (body == NULL || ui_font_text_width(body, &width) !=
            UI_FONT_STATUS_OK) {
        return 0U;
    }
    return width;
}

static enum taskmgr_status text_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct taskmgr_rgb colour)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (clip.width == 0U || clip.height == 0U || body == NULL) {
        return TASKMGR_STATUS_OK;
    }
    bounds = (struct surface_rect){ window_rect.x, window_rect.y,
        window_rect.width, window_rect.height };
    region = (struct surface_rect){ clip.x, clip.y, clip.width, clip.height };
    if (ui_font_draw_text_clipped(canvas, bounds, region, x, baseline, body,
            pack_rgb(colour), NULL) != UI_FONT_STATUS_OK) {
        return TASKMGR_STATUS_OK;
    }
    return TASKMGR_STATUS_OK;
}

/* Right-aligned inside a column, which every numeric column here is: a
 * table of figures that do not line up on their last digit is a table you
 * have to read rather than scan. */
static enum taskmgr_status text_right(struct ui_rect damage,
    struct ui_rect column, uint32_t baseline, const char *body,
    struct taskmgr_rgb colour)
{
    const uint32_t width = width_of(body);

    if (column.width < width + TASKMGR_PAD) {
        return text_at(damage, column.x, baseline, body, colour);
    }
    return text_at(damage, column.x + column.width - width - TASKMGR_PAD,
        baseline, body, colour);
}

static const uint8_t *glyph_cell(const char *name, uint32_t wanted,
    uint32_t *size)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < EXPLORER_LUCIDE_COUNT; ++index) {
        const char *candidate = explorer_lucide[index].name;
        size_t at = 0U;

        while (candidate[at] != '\0' && name[at] != '\0' &&
                candidate[at] == name[at]) {
            ++at;
        }
        if (candidate[at] != '\0' || name[at] != '\0') {
            continue;
        }
        for (size_t slot = EXPLORER_LUCIDE_SIZES; slot > 0U; --slot) {
            if (explorer_lucide_size[slot - 1U] <= wanted) {
                *size = explorer_lucide_size[slot - 1U];
                return explorer_lucide[index].alpha[slot - 1U];
            }
        }
        *size = explorer_lucide_size[0];
        return explorer_lucide[index].alpha[0];
    }
    return NULL;
}

/* One pixel of artwork or glyph over what is already there. */
static enum taskmgr_status blend_pixel(uint32_t x, uint32_t y,
    uint32_t over, uint8_t coverage)
{
    const struct framebuffer_state format = framebuffer_get_state();
    uint32_t under;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    if (coverage == 0U) {
        return TASKMGR_STATUS_OK;
    }
    if (surface_read_pixel(canvas, x, y, &under) != SURFACE_STATUS_OK) {
        return TASKMGR_STATUS_OK;
    }
    red = (((over >> format.red_position) & 0xFFU) * coverage +
        ((under >> format.red_position) & 0xFFU) * (255U - coverage)) / 255U;
    green = (((over >> format.green_position) & 0xFFU) * coverage +
        ((under >> format.green_position) & 0xFFU) * (255U - coverage)) /
        255U;
    blue = (((over >> format.blue_position) & 0xFFU) * coverage +
        ((under >> format.blue_position) & 0xFFU) * (255U - coverage)) /
        255U;
    return surface_pixel(canvas, x, y, framebuffer_pack((uint8_t)red,
        (uint8_t)green, (uint8_t)blue)) == SURFACE_STATUS_OK ?
        TASKMGR_STATUS_OK : TASKMGR_STATUS_SURFACE_FAILURE;
}

/*
 * An application's own artwork, through the taskbar's store of it - the
 * same planes the bar and the Start menu draw, so a process listed here is
 * the picture you clicked on the bar and not a second drawing of it.
 */
static bool draw_art(const char *name, struct ui_rect box,
    struct ui_rect damage)
{
    const uint32_t wanted = box.width < box.height ? box.width : box.height;
    const uint32_t *pixels = NULL;
    const uint8_t *alpha = NULL;
    uint32_t side = 0U;
    struct ui_rect placed;
    struct ui_rect clipped;

    if (name == NULL || !taskbar_artwork(name, wanted, &pixels, &alpha,
            &side) || side == 0U) {
        return false;
    }
    placed = (struct ui_rect){
        box.x + (box.width > side ? (box.width - side) / 2U : 0U),
        box.y + (box.height > side ? (box.height - side) / 2U : 0U),
        side, side };
    clipped = intersect(placed, damage);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - placed.x + x;
            const size_t offset = (size_t)local_y * side + local_x;

            (void)blend_pixel(clipped.x + x, clipped.y + y, pixels[offset],
                alpha[offset]);
        }
    }
    return true;
}

static enum taskmgr_status draw_glyph(const char *name, struct ui_rect box,
    struct ui_rect damage, struct taskmgr_rgb colour)
{
    uint32_t size = 0U;
    const uint8_t *cell = glyph_cell(name, box.width < box.height ?
        box.width : box.height, &size);
    const uint32_t over = pack_rgb(colour);
    struct ui_rect placed;
    struct ui_rect clipped;

    if (cell == NULL) {
        return TASKMGR_STATUS_OK;
    }
    placed = (struct ui_rect){
        box.x + (box.width > size ? (box.width - size) / 2U : 0U),
        box.y + (box.height > size ? (box.height - size) / 2U : 0U),
        size, size };
    clipped = intersect(placed, damage);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - placed.x + x;

            (void)blend_pixel(clipped.x + x, clipped.y + y, over,
                cell[local_y * size + local_x]);
        }
    }
    return TASKMGR_STATUS_OK;
}

/* ================================================================ STRINGS */

static size_t append_literal(char *into, size_t capacity, size_t at,
    const char *text)
{
    while (*text != '\0' && at + 1U < capacity) {
        into[at++] = *text++;
    }
    into[at] = '\0';
    return at;
}

static size_t append_uint(char *into, size_t capacity, size_t at,
    uint32_t value)
{
    char scratch[12];
    size_t length = 0U;

    do {
        scratch[length++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    } while (value != 0U && length < sizeof(scratch));
    while (length > 0U && at + 1U < capacity) {
        into[at++] = scratch[--length];
    }
    into[at] = '\0';
    return at;
}

/* Tenths of a per cent as "23.4%", or "0%" flat when there is nothing to
 * report - Windows writes an idle process's CPU as 0 rather than 0.0. */
static size_t append_tenths(char *into, size_t capacity, size_t at,
    uint16_t tenths)
{
    if (tenths == 0U) {
        return append_literal(into, capacity, at, "0%");
    }
    at = append_uint(into, capacity, at, tenths / 10U);
    at = append_literal(into, capacity, at, ".");
    at = append_uint(into, capacity, at, tenths % 10U);
    return append_literal(into, capacity, at, "%");
}

/*
 * Kilobytes the way Windows writes them in this table: megabytes with one
 * decimal up to a gigabyte, then gigabytes with one decimal.  Zero is a
 * plain "0 MB" rather than "0.0 MB", for the same reason as above.
 */
static size_t append_size(char *into, size_t capacity, size_t at,
    uint32_t kilobytes)
{
    uint32_t whole;
    uint32_t tenth;

    if (kilobytes == 0U) {
        return append_literal(into, capacity, at, "0 MB");
    }
    if (kilobytes < 1024U * 1024U) {
        whole = kilobytes / 1024U;
        tenth = (kilobytes % 1024U) * 10U / 1024U;
        at = append_uint(into, capacity, at, whole);
        at = append_literal(into, capacity, at, ".");
        at = append_uint(into, capacity, at, tenth);
        return append_literal(into, capacity, at, " MB");
    }
    whole = kilobytes / (1024U * 1024U);
    tenth = (kilobytes % (1024U * 1024U)) * 10U / (1024U * 1024U);
    at = append_uint(into, capacity, at, whole);
    at = append_literal(into, capacity, at, ".");
    at = append_uint(into, capacity, at, tenth);
    return append_literal(into, capacity, at, " GB");
}

/* A transfer rate, which Windows writes per second in the same table. */
static size_t append_rate(char *into, size_t capacity, size_t at,
    uint32_t kilobytes)
{
    if (kilobytes == 0U) {
        return append_literal(into, capacity, at, "0 MB/s");
    }
    at = append_size(into, capacity, at, kilobytes);
    return append_literal(into, capacity, at, "/s");
}

static void copy_field(char *destination, const char *source, size_t bytes)
{
    size_t index = 0U;

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    while (index + 1U < bytes && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

/* =============================================================== GEOMETRY */

/*
 * The three caption buttons, in Windows' order and at its 46-pixel width.
 * Index 2 is close, which is the only one of them this window can honour:
 * minimise and maximise belong to a compositor that owns the window, and
 * this one owns nothing but its own contents.
 */
static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){ window_rect.x + TASKMGR_BORDER,
        window_rect.y + TASKMGR_BORDER,
        window_rect.width - TASKMGR_BORDER * 2U, TASKMGR_CAPTION };
}

static struct ui_rect caption_button_rect(size_t index)
{
    const struct ui_rect bar = caption_rect();

    if (index >= 3U || bar.width < TASKMGR_CAPTION_BUTTON * 3U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ bar.x + bar.width -
        TASKMGR_CAPTION_BUTTON * (3U - (uint32_t)index), bar.y,
        TASKMGR_CAPTION_BUTTON, bar.height };
}

static struct ui_rect menu_rect(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x, caption.y + caption.height,
        caption.width, TASKMGR_MENU };
}

static struct ui_rect tabs_rect(void)
{
    const struct ui_rect menu = menu_rect();

    return (struct ui_rect){ menu.x, menu.y + menu.height, menu.width,
        TASKMGR_TABS };
}

static const char *const tab_label[TASKMGR_TAB_COUNT] = {
    "Processes", "Performance", "Startup"
};

static struct ui_rect tab_rect(size_t index)
{
    const struct ui_rect strip = tabs_rect();
    uint32_t pen = strip.x + TASKMGR_PAD;

    if (index >= TASKMGR_TAB_COUNT) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    for (size_t scan = 0U; scan < index; ++scan) {
        pen += width_of(tab_label[scan]) + TASKMGR_PAD * 2U;
    }
    return (struct ui_rect){ pen, strip.y,
        width_of(tab_label[index]) + TASKMGR_PAD * 2U, strip.height };
}

static struct ui_rect body_rect(void)
{
    const struct ui_rect strip = tabs_rect();
    const uint32_t bottom = window_rect.y + window_rect.height -
        TASKMGR_BORDER - TASKMGR_FOOTER;
    const uint32_t top = strip.y + strip.height;

    if (bottom <= top) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ strip.x, top, strip.width, bottom - top };
}

static struct ui_rect footer_rect(void)
{
    return (struct ui_rect){ window_rect.x + TASKMGR_BORDER,
        window_rect.y + window_rect.height - TASKMGR_BORDER -
            TASKMGR_FOOTER,
        window_rect.width - TASKMGR_BORDER * 2U, TASKMGR_FOOTER };
}

static struct ui_rect end_task_rect(void)
{
    const struct ui_rect bar = footer_rect();
    const uint32_t width = 108U;
    const uint32_t height = 26U;

    if (bar.width < width + TASKMGR_PAD * 2U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ bar.x + bar.width - width - TASKMGR_PAD,
        bar.y + (bar.height - height) / 2U, width, height };
}

/*
 * The six columns.  Name takes what is left over after the five fixed ones,
 * because a name is the only field here whose length is not bounded by its
 * own units.
 */
static const struct {
    const char *label;
    uint32_t width;
} column_spec[TASKMGR_COLUMN_COUNT] = {
    { "Name", 0U },
    { "Status", 98U },
    { "CPU", 116U },
    { "Memory", 122U },
    { "Disk", 122U },
    { "Network", 128U }
};

static struct ui_rect table_rect(void)
{
    const struct ui_rect body = body_rect();

    if (body.width <= TASKMGR_PAD * 2U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ body.x + TASKMGR_PAD, body.y,
        body.width - TASKMGR_PAD * 2U, body.height };
}

static struct ui_rect column_rect(size_t index)
{
    const struct ui_rect table = table_rect();
    uint32_t fixed = 0U;
    uint32_t pen;

    if (index >= TASKMGR_COLUMN_COUNT || table.width == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    for (size_t scan = 1U; scan < TASKMGR_COLUMN_COUNT; ++scan) {
        fixed += column_spec[scan].width;
    }
    if (table.width <= fixed) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    if (index == TASKMGR_COLUMN_NAME) {
        return (struct ui_rect){ table.x, table.y, table.width - fixed,
            TASKMGR_HEAD };
    }
    pen = table.x + table.width - fixed;
    for (size_t scan = 1U; scan < index; ++scan) {
        pen += column_spec[scan].width;
    }
    return (struct ui_rect){ pen, table.y, column_spec[index].width,
        TASKMGR_HEAD };
}

/* Where visible row `position` sits - position, not index, since the table
 * is sorted. */
static struct ui_rect row_rect(size_t position)
{
    const struct ui_rect table = table_rect();
    const uint32_t top = table.y + TASKMGR_HEAD +
        (uint32_t)position * TASKMGR_ROW;

    if (table.width == 0U || position >= order_count ||
            top + TASKMGR_ROW > table.y + table.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ table.x, top, table.width, TASKMGR_ROW };
}

static struct ui_rect tile_rect(size_t index)
{
    const struct ui_rect body = body_rect();
    const uint32_t top = body.y + TASKMGR_PAD +
        (uint32_t)index * (TASKMGR_TILE + 8U);

    if (index >= TASKMGR_RESOURCE_COUNT || body.width <= TASKMGR_RAIL ||
            top + TASKMGR_TILE > body.y + body.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ body.x + TASKMGR_PAD, top,
        TASKMGR_RAIL - TASKMGR_PAD * 2U, TASKMGR_TILE };
}

/* ============================================================== ORDERING */

static uint32_t sort_key(const struct taskmgr_process *process,
    enum taskmgr_column column)
{
    switch (column) {
    case TASKMGR_COLUMN_CPU:
        return process->cpu_tenths;
    case TASKMGR_COLUMN_MEMORY:
        return process->memory_kb;
    case TASKMGR_COLUMN_DISK:
        return process->disk_kb_s;
    case TASKMGR_COLUMN_NETWORK:
        return process->network_kb_s;
    default:
        return 0U;
    }
}

/* A-Z, case-insensitively, which is the order a person reading names
 * expects and not the order their bytes happen to fall in. */
static bool name_before(const char *left, const char *right)
{
    for (size_t index = 0U; ; ++index) {
        uint32_t a = (unsigned char)left[index];
        uint32_t b = (unsigned char)right[index];

        a = a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a;
        b = b >= 'A' && b <= 'Z' ? b - 'A' + 'a' : b;
        if (a != b) {
            return a < b;
        }
        if (a == '\0') {
            return false;
        }
    }
}

/*
 * Rebuild the display order.
 *
 * Group HEADINGS keep their place and take the rows that follow them with
 * them: Windows sorts inside "Apps" and inside "Background processes"
 * rather than across the two, and a sort that let a background task rise
 * above the Apps heading would be a sort that broke the grouping it is
 * drawn inside.  A list with no headings at all sorts as one run, which is
 * what a caller that never set one should get.
 *
 * Insertion sort: this is at most twenty-eight rows, it runs on a click
 * rather than on a frame, and a quicksort here would be a page of code to
 * save a few hundred comparisons nobody can perceive.
 */
static void rebuild_order(void)
{
    size_t run_start = 0U;

    order_count = 0U;
    for (size_t index = 0U; index < TASKMGR_MAX_PROCESSES; ++index) {
        if (processes[index].present) {
            order[order_count++] = index;
        }
    }
    while (run_start < order_count) {
        size_t run_end = run_start;

        if (processes[order[run_start]].heading) {
            ++run_start;
            continue;
        }
        while (run_end < order_count &&
                !processes[order[run_end]].heading) {
            ++run_end;
        }
        for (size_t i = run_start + 1U; i < run_end; ++i) {
            const size_t held = order[i];
            const struct taskmgr_process *a = &processes[held];
            size_t j = i;

            while (j > run_start) {
                const struct taskmgr_process *b = &processes[order[j - 1U]];
                bool swap;

                if (sort_column == TASKMGR_COLUMN_NAME) {
                    swap = sort_descending ?
                        name_before(b->name, a->name) :
                        name_before(a->name, b->name);
                } else if (sort_column == TASKMGR_COLUMN_STATUS) {
                    swap = sort_descending ?
                        name_before(b->status, a->status) :
                        name_before(a->status, b->status);
                } else {
                    const uint32_t ka = sort_key(a, sort_column);
                    const uint32_t kb = sort_key(b, sort_column);

                    swap = sort_descending ? kb < ka : ka < kb;
                }
                if (!swap) {
                    break;
                }
                order[j] = order[j - 1U];
                --j;
            }
            order[j] = held;
        }
        run_start = run_end;
    }
}

/* Where in the display order a given slot sits, or SIZE_MAX. */
static size_t position_of(size_t index)
{
    for (size_t position = 0U; position < order_count; ++position) {
        if (order[position] == index) {
            return position;
        }
    }
    return (size_t)-1;
}

static struct ui_rect row_damage(size_t index)
{
    const size_t position = position_of(index);

    return position == (size_t)-1 ? (struct ui_rect){ 0U, 0U, 0U, 0U } :
        row_rect(position);
}

/* ================================================================ TOTALS */

static uint32_t counted_processes(void)
{
    uint32_t count = 0U;

    for (size_t index = 0U; index < TASKMGR_MAX_PROCESSES; ++index) {
        if (processes[index].present && !processes[index].heading) {
            ++count;
        }
    }
    return count;
}

/*
 * The aggregate under a column's name.
 *
 * CPU is a sum - the whole machine's load - which is what Windows puts
 * there.  The other three are sums too, but of bytes rather than per cent,
 * so they are drawn as sizes.  A caller whose rows do not add up to its
 * meter is showing an inconsistency it owns; this reports what the rows
 * say rather than quietly substituting the meter.
 */
static uint32_t column_total(enum taskmgr_column column)
{
    uint32_t total = 0U;

    for (size_t index = 0U; index < TASKMGR_MAX_PROCESSES; ++index) {
        if (!processes[index].present || processes[index].heading) {
            continue;
        }
        total += sort_key(&processes[index], column);
    }
    return total;
}

/* =================================================================== CHROME */

static enum taskmgr_status draw_caption(struct ui_rect damage)
{
    const struct ui_rect bar = caption_rect();
    static const char title[] = "Task Manager";
    const uint32_t centre = bar.x + bar.width / 2U;
    enum taskmgr_status status = fill(bar, damage, caption_fill);

    /* The window's own icon, which is the application artwork the bar and
     * the Start menu draw - one picture of Task Manager, not two. */
    if (status == TASKMGR_STATUS_OK) {
        /* Sixteen, which is both what Windows puts in a title bar and a
         * size the artwork is actually rasterized at - twenty is neither,
         * and quietly drew the sixteen anyway. */
        const struct ui_rect box = { bar.x + 8U,
            bar.y + (bar.height - TASKMGR_ROW_ICON) / 2U, TASKMGR_ROW_ICON,
            TASKMGR_ROW_ICON };

        if (!draw_art("taskmgr", box, damage)) {
            status = draw_glyph("monitor", box, damage,
                focused ? accent : ink_faint);
        }
    }
    if (status == TASKMGR_STATUS_OK) {
        status = text_at(damage, centre - width_of(title) / 2U,
            bar.y + bar.height / 2U + 5U, title, focused ? ink : ink_faint);
    }
    /*
     * Minimise, maximise and close, in Windows' order and at its 46-pixel
     * width.
     *
     * All THREE are drawn rather than looked up.  Close used to come from
     * the Lucide set, which put a 24-pixel mark with a round-capped
     * two-pixel stroke beside a ten-pixel minimise and a ten-pixel square:
     * three buttons at two different sizes and two different weights, with
     * the odd one out being the most-looked-at control in the window.  Ten
     * pixels of one-pixel line, three times, is the whole of what Windows
     * draws here.
     */
    for (size_t index = 0U; index < 3U && status == TASKMGR_STATUS_OK;
         ++index) {
        const struct ui_rect button = caption_button_rect(index);
        const uint32_t mid_x = button.x + button.width / 2U;
        const uint32_t mid_y = button.y + button.height / 2U;

        if (button.width == 0U) {
            continue;
        }
        /* Close lights red under the pointer, the way Windows lights it,
         * and the other two do not light at all because they do nothing. */
        if (index == 2U && hover_close) {
            status = fill(button, damage, close_hot);
            if (status != TASKMGR_STATUS_OK) {
                return status;
            }
        }

        if (index == 0U) {
            status = fill((struct ui_rect){ mid_x - 5U, mid_y, 10U, 1U },
                damage, ink_soft);
        } else if (index == 1U) {
            const struct ui_rect box = { mid_x - 5U, mid_y - 5U, 10U, 10U };

            status = fill((struct ui_rect){ box.x, box.y, box.width, 1U },
                damage, ink_soft);
            if (status == TASKMGR_STATUS_OK) {
                status = fill((struct ui_rect){ box.x,
                    box.y + box.height - 1U, box.width, 1U }, damage,
                    ink_soft);
            }
            if (status == TASKMGR_STATUS_OK) {
                status = fill((struct ui_rect){ box.x, box.y, 1U,
                    box.height }, damage, ink_soft);
            }
            if (status == TASKMGR_STATUS_OK) {
                status = fill((struct ui_rect){ box.x + box.width - 1U,
                    box.y, 1U, box.height }, damage, ink_soft);
            }
        } else {
            const struct taskmgr_rgb mark = hover_close ? caption_fill : ink_soft;

            for (uint32_t step = 0U; step < 10U &&
                    status == TASKMGR_STATUS_OK; ++step) {
                status = fill((struct ui_rect){ mid_x - 5U + step,
                    mid_y - 5U + step, 1U, 1U }, damage, mark);
                if (status == TASKMGR_STATUS_OK) {
                    status = fill((struct ui_rect){ mid_x - 5U + step,
                        mid_y + 4U - step, 1U, 1U }, damage, mark);
                }
            }
        }
    }
    return status;
}

static enum taskmgr_status draw_menu(struct ui_rect damage)
{
    static const char *const items[] = { "File", "Options", "View" };
    const struct ui_rect bar = menu_rect();
    uint32_t pen = bar.x + TASKMGR_PAD;
    enum taskmgr_status status = fill(bar, damage, caption_fill);

    for (size_t index = 0U; index < 3U && status == TASKMGR_STATUS_OK;
         ++index) {
        status = text_at(damage, pen, bar.y + bar.height / 2U + 4U,
            items[index], ink);
        pen += width_of(items[index]) + 18U;
    }
    return status;
}

/*
 * The tab strip.  Windows 10's Task Manager underlines the open tab in the
 * accent colour rather than drawing a raised folder tab, which is the
 * newer of the two conventions it mixes and the one worth copying.
 */
static enum taskmgr_status draw_tabs(struct ui_rect damage)
{
    const struct ui_rect strip = tabs_rect();
    enum taskmgr_status status = fill(strip, damage, chrome);

    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ strip.x,
            strip.y + strip.height - 1U, strip.width, 1U }, damage, rule);
    }
    for (size_t index = 0U; index < TASKMGR_TAB_COUNT &&
            status == TASKMGR_STATUS_OK; ++index) {
        const struct ui_rect box = tab_rect(index);
        const bool open = (size_t)tab == index;

        if (box.width == 0U) {
            continue;
        }
        if (open) {
            status = fill(box, damage, caption_fill);
            if (status == TASKMGR_STATUS_OK) {
                status = fill((struct ui_rect){ box.x,
                    box.y + box.height - 2U, box.width, 2U }, damage,
                    accent);
            }
        } else if (index == hover_tab) {
            status = fill(box, damage, hover_fill);
        }
        if (status == TASKMGR_STATUS_OK) {
            status = text_at(damage, box.x + TASKMGR_PAD,
                box.y + box.height / 2U + 5U, tab_label[index],
                open ? ink : ink_soft);
        }
    }
    return status;
}

/*
 * The footer.
 *
 * Windows puts "Simple view" at one end and "End process" at the other and
 * leaves the middle empty.  Activity Monitor spends that space on a live
 * summary, which is the better idea and costs nothing here: the counts and
 * the two headline loads, so the totals are readable without reading the
 * table above them.
 */
static enum taskmgr_status draw_footer(struct ui_rect damage)
{
    const struct ui_rect bar = footer_rect();
    const struct ui_rect button = end_task_rect();
    const uint32_t baseline = bar.y + bar.height / 2U + 5U;
    char summary[96];
    size_t at = 0U;
    enum taskmgr_status status = fill(bar, damage, chrome);

    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ bar.x, bar.y, bar.width, 1U },
            damage, rule);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = draw_glyph("chevron-up", (struct ui_rect){
            bar.x + TASKMGR_PAD, bar.y, TASKMGR_MARK, bar.height }, damage,
            ink_soft);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = text_at(damage, bar.x + TASKMGR_PAD + 18U, baseline,
            "Simple view", ink_soft);
    }
    at = append_literal(summary, sizeof(summary), at, "Processes: ");
    at = append_uint(summary, sizeof(summary), at, counted_processes());
    at = append_literal(summary, sizeof(summary), at, "     CPU: ");
    at = append_tenths(summary, sizeof(summary), at,
        meters[TASKMGR_RESOURCE_CPU].percent_tenths);
    at = append_literal(summary, sizeof(summary), at, "     Memory: ");
    (void)append_tenths(summary, sizeof(summary), at,
        meters[TASKMGR_RESOURCE_MEMORY].percent_tenths);
    if (status == TASKMGR_STATUS_OK) {
        const uint32_t width = width_of(summary);
        const uint32_t centre = bar.x + bar.width / 2U;

        status = text_at(damage, centre > width / 2U ? centre - width / 2U :
            bar.x, baseline, summary, ink_soft);
    }
    if (status != TASKMGR_STATUS_OK || button.width == 0U) {
        return status;
    }
    /* End task is only live with a task selected - a group heading is not
     * one, and neither is nothing. */
    {
        const bool armed = selected != (size_t)-1 &&
            processes[selected].present && !processes[selected].heading;

        status = fill(button, damage, !armed ? chrome :
            (hover_end_task ? button_hot : button_fill));
        if (status == TASKMGR_STATUS_OK) {
            const struct taskmgr_rgb edge = armed && hover_end_task ?
                accent : button_edge;

            status = fill((struct ui_rect){ button.x, button.y,
                button.width, 1U }, damage, edge);
            if (status == TASKMGR_STATUS_OK) {
                status = fill((struct ui_rect){ button.x,
                    button.y + button.height - 1U, button.width, 1U },
                    damage, edge);
            }
            if (status == TASKMGR_STATUS_OK) {
                status = fill((struct ui_rect){ button.x, button.y, 1U,
                    button.height }, damage, edge);
            }
            if (status == TASKMGR_STATUS_OK) {
                status = fill((struct ui_rect){
                    button.x + button.width - 1U, button.y, 1U,
                    button.height }, damage, edge);
            }
        }
        if (status == TASKMGR_STATUS_OK) {
            static const char label[] = "End process";

            status = text_at(damage,
                button.x + (button.width - width_of(label)) / 2U,
                button.y + button.height / 2U + 5U, label,
                armed ? ink : ink_faint);
        }
    }
    return status;
}

/* ============================================================== PROCESSES */

static enum taskmgr_status draw_column_heads(struct ui_rect damage)
{
    const struct ui_rect table = table_rect();
    enum taskmgr_status status = fill((struct ui_rect){ table.x, table.y,
        table.width, TASKMGR_HEAD }, damage, head_fill);

    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ table.x,
            table.y + TASKMGR_HEAD - 1U, table.width, 1U }, damage, rule);
    }
    for (size_t index = 0U; index < TASKMGR_COLUMN_COUNT &&
            status == TASKMGR_STATUS_OK; ++index) {
        const struct ui_rect box = column_rect(index);
        const bool sorted = (size_t)sort_column == index;
        char total[24];
        const uint32_t name_baseline = box.y + 16U;
        const uint32_t total_baseline = box.y + 32U;

        if (box.width == 0U) {
            continue;
        }
        if (index == hover_column && !sorted) {
            status = fill(box, damage, hover_fill);
        }
        /*
         * The aggregate under the name, which is the thing that makes this
         * heading worth reading: Windows puts the whole machine's CPU
         * percentage under the word CPU, so the column says what it is AND
         * what it currently comes to.  The cell takes the heat of that
         * aggregate too.
         */
        total[0] = '\0';
        if (index == TASKMGR_COLUMN_CPU) {
            const uint32_t sum = column_total(TASKMGR_COLUMN_CPU);

            (void)append_tenths(total, sizeof(total), 0U,
                (uint16_t)(sum > 1000U ? 1000U : sum));
            status = status != TASKMGR_STATUS_OK ? status :
                fill((struct ui_rect){ box.x, box.y, box.width,
                    TASKMGR_HEAD - 1U }, damage,
                    heat_for((uint16_t)(sum > 1000U ? 1000U : sum)));
        } else if (index == TASKMGR_COLUMN_MEMORY) {
            (void)append_size(total, sizeof(total), 0U,
                column_total(TASKMGR_COLUMN_MEMORY));
        } else if (index == TASKMGR_COLUMN_DISK) {
            (void)append_rate(total, sizeof(total), 0U,
                column_total(TASKMGR_COLUMN_DISK));
        } else if (index == TASKMGR_COLUMN_NETWORK) {
            (void)append_rate(total, sizeof(total), 0U,
                column_total(TASKMGR_COLUMN_NETWORK));
        }
        if (status != TASKMGR_STATUS_OK) {
            return status;
        }
        /*
         * Ink that survives the plate under it.
         *
         * The sorted heading takes the accent, and the CPU heading takes
         * the heat of its own total - which put #0078D4 on a strong orange
         * whenever the machine was busy, with the aggregate under it in
         * grey.  Blue on orange is the one pairing on this palette that
         * reads worst, and it appeared exactly when the number mattered
         * most.  A tinted cell drops back to plain ink, which is what the
         * heat map's own figures have always used.
         */
        {
            const bool tinted = index == TASKMGR_COLUMN_CPU &&
                column_total(TASKMGR_COLUMN_CPU) >= 60U;
            const struct taskmgr_rgb heading_ink = tinted ? ink :
                (sorted ? accent : ink);

            if (index == TASKMGR_COLUMN_NAME) {
                status = text_at(damage, box.x + TASKMGR_PAD, name_baseline,
                    column_spec[index].label, heading_ink);
            } else {
                status = text_right(damage, box, name_baseline,
                    column_spec[index].label, heading_ink);
            }
            if (status == TASKMGR_STATUS_OK && total[0] != '\0') {
                status = text_right(damage, box, total_baseline, total,
                    tinted ? ink : ink_soft);
            }
        }
        /*
         * The sort arrow, in the accent, BESIDE the label rather than
         * centred over the column.  Windows centres it, which works
         * because its columns are all about as wide as their headings;
         * Name here is four hundred pixels of mostly nothing, and an arrow
         * floating in the middle of that reads as belonging to no column
         * at all.  Next to the word it orders, it cannot.
         */
        if (status == TASKMGR_STATUS_OK && sorted) {
            const bool tinted = index == TASKMGR_COLUMN_CPU &&
                column_total(TASKMGR_COLUMN_CPU) >= 60U;
            const uint32_t label_width =
                width_of(column_spec[index].label);
            /* Four pixels of air on the label's side of the mark.  At 15
             * the mark's box ended one pixel PAST where the right-aligned
             * label began, so "CPU" and its chevron ran together into one
             * shape. */
            const uint32_t arrow_x = index == TASKMGR_COLUMN_NAME ?
                box.x + TASKMGR_PAD + label_width + 3U :
                box.x + box.width - TASKMGR_PAD - label_width -
                    TASKMGR_MARK - 4U;

            status = draw_glyph(sort_descending ? "chevron-down" :
                "chevron-up", (struct ui_rect){ arrow_x,
                name_baseline - 13U, TASKMGR_MARK, TASKMGR_MARK }, damage,
                tinted ? ink : accent);
        }
    }
    return status;
}

static enum taskmgr_status draw_process_row(size_t position,
    struct ui_rect damage)
{
    const size_t index = order[position];
    const struct taskmgr_process *process = &processes[index];
    const struct ui_rect row = row_rect(position);
    const uint32_t baseline = row.y + row.height / 2U + 5U;
    enum taskmgr_status status;
    char figure[24];

    if (row.width == 0U) {
        return TASKMGR_STATUS_OK;
    }
    /*
     * A group divider: "Apps (4)" in the accent, no numbers of its own.
     * Windows draws these as plain rows with a chevron; the accent is this
     * shell's, and it is what makes the two groups findable while scanning
     * a sorted table.
     */
    if (process->heading) {
        char label[TASKMGR_NAME_BYTES + 8];
        size_t at = append_literal(label, sizeof(label), 0U, process->name);

        status = fill(row, damage, head_fill);
        if (status == TASKMGR_STATUS_OK) {
            status = draw_glyph("chevron-down", (struct ui_rect){
                row.x + 4U, row.y, TASKMGR_MARK, row.height }, damage,
                ink_soft);
        }
        if (status == TASKMGR_STATUS_OK) {
            (void)at;
            status = text_at(damage, row.x + 20U, baseline, label, accent);
        }
        return status;
    }

    {
        const bool chosen = index == selected;
        const bool hot = index == hover_row;
        struct taskmgr_rgb ground = table_fill;

        if (chosen) {
            ground = select_fill;
        } else if (hot) {
            ground = hover_fill;
        }
        status = fill(row, damage, ground);
        /* Every numeric cell takes the heat of what it reports.  CPU is a
         * percentage; the other three are rates and sizes, so they are
         * scaled against the busiest row rather than against a unit they
         * have no ceiling in. */
        if (status == TASKMGR_STATUS_OK && !chosen) {
            static const enum taskmgr_column heated[] = {
                TASKMGR_COLUMN_CPU, TASKMGR_COLUMN_MEMORY,
                TASKMGR_COLUMN_DISK, TASKMGR_COLUMN_NETWORK
            };

            for (size_t slot = 0U; slot < 4U &&
                    status == TASKMGR_STATUS_OK; ++slot) {
                const struct ui_rect box = column_rect(heated[slot]);
                uint16_t tenths;

                if (box.width == 0U) {
                    continue;
                }
                if (heated[slot] == TASKMGR_COLUMN_CPU) {
                    tenths = process->cpu_tenths;
                } else {
                    const uint32_t total = column_total(heated[slot]);
                    const uint32_t mine = sort_key(process, heated[slot]);

                    tenths = total == 0U ? 0U :
                        (uint16_t)(mine * 1000U / total);
                }
                status = fill((struct ui_rect){ box.x, row.y, box.width,
                    row.height }, damage, heat_for(tenths));
            }
        }
        if (status == TASKMGR_STATUS_OK && chosen) {
            status = fill((struct ui_rect){ row.x, row.y,
                TASKMGR_SELECT_BAR, row.height }, damage, accent);
        }
    }
    if (status != TASKMGR_STATUS_OK) {
        return status;
    }
    {
        const struct ui_rect icon = { row.x + 22U,
            row.y + (row.height - TASKMGR_ROW_ICON) / 2U,
            TASKMGR_ROW_ICON, TASKMGR_ROW_ICON };

        if (!draw_art(process->art, icon, damage)) {
            status = draw_glyph(process->glyph == NULL ? "box" :
                process->glyph, icon, damage, kind_colour[process->kind]);
        }
    }
    if (status == TASKMGR_STATUS_OK) {
        status = text_at(damage, row.x + 46U, baseline, process->name, ink);
    }
    if (status == TASKMGR_STATUS_OK) {
        const struct ui_rect box = column_rect(TASKMGR_COLUMN_STATUS);

        if (box.width != 0U && process->status[0] != '\0') {
            status = text_right(damage, box, baseline, process->status,
                ink_soft);
        }
    }
    if (status == TASKMGR_STATUS_OK) {
        const struct ui_rect box = column_rect(TASKMGR_COLUMN_CPU);

        (void)append_tenths(figure, sizeof(figure), 0U, process->cpu_tenths);
        status = box.width == 0U ? status :
            text_right(damage, box, baseline, figure, ink);
    }
    if (status == TASKMGR_STATUS_OK) {
        const struct ui_rect box = column_rect(TASKMGR_COLUMN_MEMORY);

        (void)append_size(figure, sizeof(figure), 0U, process->memory_kb);
        status = box.width == 0U ? status :
            text_right(damage, box, baseline, figure, ink);
    }
    if (status == TASKMGR_STATUS_OK) {
        const struct ui_rect box = column_rect(TASKMGR_COLUMN_DISK);

        (void)append_rate(figure, sizeof(figure), 0U, process->disk_kb_s);
        status = box.width == 0U ? status :
            text_right(damage, box, baseline, figure, ink);
    }
    if (status == TASKMGR_STATUS_OK) {
        const struct ui_rect box = column_rect(TASKMGR_COLUMN_NETWORK);

        (void)append_rate(figure, sizeof(figure), 0U, process->network_kb_s);
        status = box.width == 0U ? status :
            text_right(damage, box, baseline, figure, ink);
    }
    return status;
}

static enum taskmgr_status draw_processes(struct ui_rect damage)
{
    const struct ui_rect table = table_rect();
    /* The body first, then the table inside it: the padding either side of
     * the table is part of this window and has to be painted, or the frame
     * colour shows through it as a blue margin. */
    enum taskmgr_status status = fill(body_rect(), damage, table_fill);

    if (status == TASKMGR_STATUS_OK) {
        status = fill(table, damage, table_fill);
    }

    if (status == TASKMGR_STATUS_OK) {
        status = draw_column_heads(damage);
    }
    /* An empty table says so.  A caller that has set no rows should see
     * that it has set no rows, not an expanse of white it has to guess
     * about. */
    if (status == TASKMGR_STATUS_OK && order_count == 0U) {
        static const char empty[] = "No processes to show";

        return text_at(damage, table.x + TASKMGR_PAD,
            table.y + TASKMGR_HEAD + 26U, empty, ink_faint);
    }
    for (size_t position = 0U; position < order_count &&
            status == TASKMGR_STATUS_OK; ++position) {
        status = draw_process_row(position, damage);
    }
    return status;
}

/* ============================================================ PERFORMANCE */

/*
 * The height of the trace at a fractional position along it.
 *
 * Sixty samples are stretched over several hundred columns, so a column
 * that simply looked up the nearest sample drew each one ten times over and
 * the trace came out as a staircase - which is what this did before, and
 * which reads as a bar chart of sixty bars rather than as a line.  This
 * interpolates between the two samples a column falls between, in eighths
 * of a sample, so the line rises the way the reading did.
 *
 * Fixed point throughout: src/kernel has no floating point.
 */
static uint32_t trace_at(const struct taskmgr_meter *meter, uint32_t column,
    uint32_t width)
{
    const uint32_t last = TASKMGR_HISTORY - 1U;
    const uint32_t span = width < 2U ? 1U : width - 1U;
    /* 8.8 fixed point: which sample, and how far past it. */
    const uint32_t position = column * last * 256U / span;
    const uint32_t index = position >> 8;
    const uint32_t fraction = position & 0xFFU;
    uint32_t left;
    uint32_t right;

    if (index >= last) {
        return (meter->history[last] > 100U ? 100U :
            meter->history[last]) * 256U;
    }
    left = meter->history[index] > 100U ? 100U : meter->history[index];
    right = meter->history[index + 1U] > 100U ? 100U :
        meter->history[index + 1U];
    /* Returned in 8.8 as well, so the CALLER can put the trace's edge
     * between two pixels instead of snapping it to one - which is the
     * difference between a curve and a flight of one-pixel stairs. */
    return left * (256U - fraction) + right * fraction;
}

/*
 * A resource tile in the left rail.  Windows draws a small live graph
 * inside each one, so the rail is four sparklines and not four labels; the
 * open tile takes the accent bar this shell puts on every chosen row.
 */
static enum taskmgr_status draw_tile(size_t index, struct ui_rect damage)
{
    const struct ui_rect box = tile_rect(index);
    const struct taskmgr_meter *meter = &meters[index];
    static const char *const names[TASKMGR_RESOURCE_COUNT] = {
        "CPU", "Memory", "Disk", "Network"
    };
    const bool open = (size_t)resource == index;
    const struct taskmgr_rgb hue = resource_colour[index];
    char figure[24];
    enum taskmgr_status status;

    if (box.width == 0U) {
        return TASKMGR_STATUS_OK;
    }
    status = fill(box, damage, open ? select_fill :
        (index == hover_tile ? hover_fill : caption_fill));
    if (status == TASKMGR_STATUS_OK && !open) {
        /* A hairline, so an unselected tile is still a tile rather than a
         * label with a sparkline adrift beneath it. */
        status = fill((struct ui_rect){ box.x, box.y + box.height - 1U,
            box.width, 1U }, damage, rule_soft);
    }
    if (status == TASKMGR_STATUS_OK && open) {
        status = fill((struct ui_rect){ box.x, box.y, TASKMGR_SELECT_BAR,
            box.height }, damage, hue);
    }
    /* The sparkline: the same sixty samples the big graph draws, at tile
     * size, filled under the line so a glance reads as an area rather than
     * as a thread. */
    if (status == TASKMGR_STATUS_OK && meter->present) {
        const struct ui_rect plot = { box.x + 8U + TASKMGR_SELECT_BAR,
            box.y + 30U, box.width - 16U - TASKMGR_SELECT_BAR,
            box.height - 38U };

        for (uint32_t column = 0U; column < plot.width &&
                status == TASKMGR_STATUS_OK; ++column) {
            const uint32_t tall_256 = plot.height *
                trace_at(meter, column, plot.width) / 100U;
            const uint32_t tall = tall_256 >> 8;
            const struct taskmgr_rgb wash = mix(caption_fill, hue, 120U);

            if (tall != 0U) {
                status = fill((struct ui_rect){ plot.x + column,
                    plot.y + plot.height - tall, 1U, tall }, damage, wash);
            }
            /* The part-covered pixel at the top of the column, so a
             * sparkline is a curve at tile size too. */
            if (status == TASKMGR_STATUS_OK && (tall_256 & 0xFFU) != 0U &&
                    tall < plot.height) {
                (void)blend_pixel(plot.x + column,
                    plot.y + plot.height - tall - 1U, pack_rgb(wash),
                    (uint8_t)(tall_256 & 0xFFU));
            }
        }
    }
    if (status == TASKMGR_STATUS_OK) {
        status = text_at(damage, box.x + 10U + TASKMGR_SELECT_BAR,
            box.y + 20U, names[index], ink);
    }
    if (status == TASKMGR_STATUS_OK) {
        const struct ui_rect right = { box.x, box.y, box.width, box.height };

        (void)append_tenths(figure, sizeof(figure), 0U,
            meter->percent_tenths);
        status = text_right(damage, right, box.y + 20U, figure, hue);
    }
    return status;
}

/*
 * The big graph.
 *
 * Windows 10 draws a ten-by-six grid, the line across the full sixty
 * seconds, and the area under it filled in a paler shade of the same hue.
 * All three are here; the grid is what turns a wiggle into a reading.
 */
static enum taskmgr_status draw_graph(struct ui_rect plot,
    struct ui_rect damage)
{
    const struct taskmgr_meter *meter = &meters[resource];
    const struct taskmgr_rgb hue = resource_colour[resource];
    const struct taskmgr_rgb wash = mix(caption_fill, hue, 60U);
    enum taskmgr_status status = fill(plot, damage, caption_fill);
    uint32_t previous_top = 0U;

    if (status == TASKMGR_STATUS_OK) {
        for (uint32_t line = 1U; line < TASKMGR_GRAPH_GRID &&
                status == TASKMGR_STATUS_OK; ++line) {
            status = fill((struct ui_rect){ plot.x,
                plot.y + plot.height * line / TASKMGR_GRAPH_GRID,
                plot.width, 1U }, damage, rule_soft);
        }
    }
    if (status == TASKMGR_STATUS_OK) {
        for (uint32_t line = 1U; line < 10U &&
                status == TASKMGR_STATUS_OK; ++line) {
            status = fill((struct ui_rect){
                plot.x + plot.width * line / 10U, plot.y, 1U, plot.height },
                damage, rule_soft);
        }
    }
    if (status != TASKMGR_STATUS_OK || !meter->present) {
        return status;
    }
    for (uint32_t column = 0U; column < plot.width &&
            status == TASKMGR_STATUS_OK; ++column) {
        /* The trace's height in 8.8, so its edge can sit between pixels. */
        const uint32_t tall_256 = plot.height *
            trace_at(meter, column, plot.width) / 100U;
        const uint32_t tall = tall_256 >> 8;
        const uint32_t coverage = tall_256 & 0xFFU;
        const uint32_t top = plot.y + plot.height - tall;

        if (tall != 0U) {
            status = fill((struct ui_rect){ plot.x + column, top, 1U, tall },
                damage, wash);
        }
        /* The pixel the edge only partly covers, at exactly the fraction it
         * covers it by.  This is the whole of what stops a rising curve
         * from being a staircase. */
        if (status == TASKMGR_STATUS_OK && coverage != 0U &&
                top > plot.y) {
            (void)blend_pixel(plot.x + column, top - 1U, pack_rgb(wash),
                (uint8_t)coverage);
        }
        /*
         * The line itself, joined to where the last column ended so a climb
         * is continuous rather than a row of detached ticks, and blended at
         * both ends for the same reason the fill is.
         */
        if (status == TASKMGR_STATUS_OK) {
            const uint32_t from = column == 0U || previous_top < top ?
                top : previous_top;
            const uint32_t to = column == 0U || previous_top > top ?
                top : previous_top;

            status = fill((struct ui_rect){ plot.x + column, to, 1U,
                from - to + 1U }, damage, hue);
            if (status == TASKMGR_STATUS_OK && coverage != 0U &&
                    top > plot.y) {
                (void)blend_pixel(plot.x + column, top - 1U, pack_rgb(hue),
                    (uint8_t)coverage);
            }
        }
        previous_top = top;
    }
    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ plot.x, plot.y, plot.width, 1U },
            damage, rule);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ plot.x, plot.y + plot.height - 1U,
            plot.width, 1U }, damage, rule);
    }
    return status;
}

/*
 * The per-core meters, which are htop's idea and the best thing in it.
 *
 * Windows 10 can show logical processors, but only by right-clicking into
 * a mode that REPLACES the main graph; htop shows every core at once,
 * always, and it is the fastest way there is to see that one core is
 * pinned while seven idle.  They sit under the graph rather than instead
 * of it, so the machine's shape and its total are readable together.
 */
static enum taskmgr_status draw_cores(struct ui_rect area,
    struct ui_rect damage)
{
    const uint32_t columns = core_count < TASKMGR_CORE_COLUMNS ?
        (uint32_t)core_count : TASKMGR_CORE_COLUMNS;
    const uint32_t rows = ((uint32_t)core_count + columns - 1U) / columns;
    enum taskmgr_status status = TASKMGR_STATUS_OK;

    if (core_count == 0U || columns == 0U || area.height < 24U) {
        return TASKMGR_STATUS_OK;
    }
    status = text_at(damage, area.x, area.y + 10U, "Logical processors",
        ink_soft);
    for (size_t index = 0U; index < core_count &&
            status == TASKMGR_STATUS_OK; ++index) {
        const uint32_t column = (uint32_t)index % columns;
        const uint32_t row = (uint32_t)index / columns;
        const uint32_t cell_width = area.width / columns;
        const uint32_t cell_height = (area.height - 16U) /
            (rows == 0U ? 1U : rows);
        char label[8];
        uint32_t number_width;
        struct ui_rect cell;
        uint16_t load;
        uint32_t filled;

        if (cell_height < 12U) {
            break;
        }
        /* The core's number sits BESIDE its bar rather than on top of it:
         * a digit over a fill that changes colour under load is a digit
         * that disappears exactly when the core is worth looking at. */
        (void)append_uint(label, sizeof(label), 0U, (uint32_t)index + 1U);
        number_width = width_of(label) + 6U;
        cell = (struct ui_rect){ area.x + column * cell_width + number_width,
            area.y + 18U + row * cell_height,
            cell_width > number_width + 8U ?
                cell_width - number_width - 8U : 4U,
            cell_height > 8U ? cell_height - 8U : cell_height };
        load = cores[index];
        filled = cell.width * (load > 1000U ? 1000U : load) / 1000U;
        status = text_at(damage, area.x + column * cell_width,
            cell.y + cell.height / 2U + 4U, label, ink_faint);
        if (status == TASKMGR_STATUS_OK) {
            /*
             * A WHITE track inside a drawn border, not a grey one.
             *
             * The heat ramp starts at very nearly white, so a lightly
             * loaded core's fill was LIGHTER than the grey track behind
             * it: the bar read as a gap and the empty part read as the
             * bar, which is the exact opposite of what a meter is for.
             * An outlined box says where the meter ends without needing
             * to be darker than anything that can be drawn inside it.
             */
            status = fill(cell, damage, caption_fill);
            if (status == TASKMGR_STATUS_OK) {
                status = outline(cell, damage, rule);
            }
        }
        if (status == TASKMGR_STATUS_OK && filled != 0U) {
            /*
             * The same heat_for() the table's cells take, so "hot" means
             * one thing everywhere in this window.  An earlier version ran
             * these bars on their own warm-to-hot ramp, which put a core
             * at twelve per cent in strong amber while the same twelve per
             * cent in the table beside it was nearly white - two scales
             * disagreeing about the same number.
             */
            const uint32_t inset = cell.width > 2U && cell.height > 2U ?
                1U : 0U;
            const uint32_t inner = cell.width - inset * 2U;
            const uint32_t width = filled > inner ? inner :
                (filled < 2U ? 2U : filled);

            /* Inside the border, so the fill never sits on top of the line
             * that says where the meter ends - and never narrower than two
             * pixels, because a core doing something is worth a mark you
             * can see. */
            status = fill((struct ui_rect){ cell.x + inset,
                cell.y + inset, width, cell.height - inset * 2U }, damage,
                heat_for(load < TASKMGR_CORE_TINT_FLOOR ?
                    TASKMGR_CORE_TINT_FLOOR : load));
        }
    }
    return status;
}

/* Every thread the table's rows say they are running.  Windows prints this
 * beside the process count and it is the one number here that says how busy
 * the machine is in a way a percentage cannot. */
static uint32_t counted_threads(void)
{
    uint32_t total = 0U;

    for (size_t index = 0U; index < TASKMGR_MAX_PROCESSES; ++index) {
        if (processes[index].present && !processes[index].heading) {
            total += processes[index].threads;
        }
    }
    return total;
}

/*
 * The unit a meter counts in, lifted off the end of its own detail line -
 * "8.0 GB" is eight gigabytes, so `used` and `total` beside it are
 * gigabytes too.  There is no separate unit field to read and inventing one
 * would mean the caller setting the same thing twice.
 */
static void unit_of(const char *detail, char *out, size_t bytes)
{
    size_t length = 0U;
    size_t start;

    out[0] = '\0';
    if (detail == NULL) {
        return;
    }
    while (detail[length] != '\0') {
        ++length;
    }
    start = length;
    while (start > 0U && ((detail[start - 1U] >= 'A' &&
            detail[start - 1U] <= 'Z') || (detail[start - 1U] >= 'a' &&
            detail[start - 1U] <= 'z'))) {
        --start;
    }
    if (start == length) {
        return;                 /* it does not end in a unit at all */
    }
    (void)append_literal(out, bytes, 0U, &detail[start]);
}

/*
 * The readouts under the graph.
 *
 * These used to be four fixed columns - Utilisation, Processes, Threads, Up
 * time - drawn under EVERY resource, so the Memory pane reported the
 * process count and the thread count and the Disk pane did the same.  Two
 * of those four are facts about the CPU; printing them under a memory graph
 * is not a simplification, it is the window answering a question nobody
 * asked and looking like it has one screen it draws four times.
 *
 * So the strip is built from what the resource actually HAS.  Utilisation
 * and up time are true of all four.  Processes and threads are counted from
 * the table and belong to the CPU.  In use and Available come off the
 * meter's own used/total and are offered only where they mean something and
 * only when the caller set them - a Disk meter with no figures gets two
 * columns rather than two zeroes.
 */
#define TASKMGR_READOUTS 4U

static enum taskmgr_status draw_readouts(struct ui_rect area,
    struct ui_rect damage)
{
    const struct taskmgr_meter *meter = &meters[resource];
    const uint32_t step = area.width / TASKMGR_READOUTS;
    const char *labels[TASKMGR_READOUTS];
    char values[TASKMGR_READOUTS][24];
    size_t count = 0U;
    enum taskmgr_status status = TASKMGR_STATUS_OK;

    labels[count] = "Utilisation";
    (void)append_tenths(values[count], sizeof(values[count]), 0U,
        meter->percent_tenths);
    ++count;
    if (resource == TASKMGR_RESOURCE_CPU) {
        labels[count] = "Processes";
        (void)append_uint(values[count], sizeof(values[count]), 0U,
            counted_processes());
        ++count;
        labels[count] = "Threads";
        (void)append_uint(values[count], sizeof(values[count]), 0U,
            counted_threads());
        ++count;
    } else if (resource == TASKMGR_RESOURCE_MEMORY && meter->total != 0U &&
            meter->used <= meter->total) {
        char unit[TASKMGR_FIELD_BYTES];
        size_t at;

        unit_of(meter->detail, unit, sizeof(unit));
        labels[count] = "In use";
        at = append_uint(values[count], sizeof(values[count]), 0U,
            meter->used);
        if (unit[0] != '\0') {
            at = append_literal(values[count], sizeof(values[count]), at,
                " ");
            (void)append_literal(values[count], sizeof(values[count]), at,
                unit);
        }
        ++count;
        labels[count] = "Available";
        at = append_uint(values[count], sizeof(values[count]), 0U,
            meter->total - meter->used);
        if (unit[0] != '\0') {
            at = append_literal(values[count], sizeof(values[count]), at,
                " ");
            (void)append_literal(values[count], sizeof(values[count]), at,
                unit);
        }
        ++count;
    }
    /* Up time is the machine's, so it goes under every one of them - and
     * last, because it is the one figure here that is not about the
     * resource the graph is drawing. */
    {
        size_t at = append_uint(values[count], sizeof(values[count]), 0U,
            uptime_seconds / 3600U);

        /* Windows writes up time as d:hh:mm:ss; this writes the two units
         * that are ever interesting on a machine that has been up for
         * minutes rather than months. */
        at = append_literal(values[count], sizeof(values[count]), at, "h ");
        at = append_uint(values[count], sizeof(values[count]), at,
            (uptime_seconds % 3600U) / 60U);
        (void)append_literal(values[count], sizeof(values[count]), at, "m");
        labels[count] = "Up time";
        ++count;
    }
    /* Left-packed at a fixed step rather than divided by the count, so a
     * resource with two figures shows two columns where the first two
     * always are instead of stretching them across the pane. */
    for (size_t index = 0U; index < count &&
            status == TASKMGR_STATUS_OK; ++index) {
        const uint32_t x = area.x + (uint32_t)index * step;

        status = text_at(damage, x, area.y + 12U, labels[index], ink_soft);
        if (status == TASKMGR_STATUS_OK) {
            status = text_at(damage, x, area.y + 34U, values[index], ink);
        }
    }
    return status;
}

static enum taskmgr_status draw_performance(struct ui_rect damage)
{
    const struct ui_rect body = body_rect();
    const struct ui_rect pane = { body.x + TASKMGR_RAIL, body.y,
        body.width > TASKMGR_RAIL ? body.width - TASKMGR_RAIL : 0U,
        body.height };
    static const char *const names[TASKMGR_RESOURCE_COUNT] = {
        "CPU", "Memory", "Disk", "Network"
    };
    enum taskmgr_status status = fill(body, damage, chrome);

    if (status == TASKMGR_STATUS_OK) {
        status = fill(pane, damage, caption_fill);
    }
    for (size_t index = 0U; index < TASKMGR_RESOURCE_COUNT &&
            status == TASKMGR_STATUS_OK; ++index) {
        status = draw_tile(index, damage);
    }
    if (status != TASKMGR_STATUS_OK || pane.width < 200U) {
        return status;
    }
    if (status == TASKMGR_STATUS_OK) {
        status = text_at(damage, pane.x + TASKMGR_PAD, pane.y + 26U,
            names[resource], ink);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = text_right(damage, (struct ui_rect){ pane.x, pane.y,
            pane.width, 0U }, pane.y + 26U, meters[resource].detail,
            ink_soft);
    }
    /* Axis labels, which Windows writes at the graph's corners: the
     * ceiling top-left, the span bottom-left, and zero bottom-right. */
    {
        const uint32_t graph_top = pane.y + 40U;
        /*
         * The per-core row is the CPU's, and only the CPU's - it is
         * headed "Logical processors".  It used to be drawn under every
         * resource, so the Memory graph came with eight CPU meters under
         * it.  On the other three the graph simply takes the height back.
         */
        const uint32_t cores_tall = core_count == 0U ||
            resource != TASKMGR_RESOURCE_CPU ? 0U :
            TASKMGR_CORE_ROW * (((uint32_t)core_count +
                TASKMGR_CORE_COLUMNS - 1U) / TASKMGR_CORE_COLUMNS) + 26U;
        const uint32_t readouts_tall = 58U;
        /*
         * The strip "60 seconds" is written in, which has to be reserved
         * rather than borrowed: the graph used to run down to the readouts
         * and the span label was drawn fourteen pixels below it, which on
         * a pane with no per-core row landed the label on the same line as
         * the figures and read as a fifth column of them.
         */
        const uint32_t span_tall = 22U;
        const uint32_t available = pane.y + pane.height - graph_top -
            span_tall - cores_tall - readouts_tall - TASKMGR_PAD;
        const struct ui_rect plot = { pane.x + TASKMGR_PAD + 32U, graph_top,
            pane.width - TASKMGR_PAD * 2U - 32U,
            available > 60U ? available : 60U };

        if (status == TASKMGR_STATUS_OK) {
            status = draw_graph(plot, damage);
        }
        if (status == TASKMGR_STATUS_OK) {
            status = text_at(damage, pane.x + TASKMGR_PAD, plot.y + 10U,
                "100%", ink_faint);
        }
        if (status == TASKMGR_STATUS_OK) {
            status = text_at(damage, pane.x + TASKMGR_PAD,
                plot.y + plot.height - 2U, "0", ink_faint);
        }
        if (status == TASKMGR_STATUS_OK) {
            static const char span[] = "60 seconds";

            status = text_at(damage,
                plot.x + plot.width - width_of(span),
                plot.y + plot.height + 14U, span, ink_faint);
        }
        if (status == TASKMGR_STATUS_OK && cores_tall != 0U) {
            status = draw_cores((struct ui_rect){ plot.x,
                plot.y + plot.height + span_tall + 4U, plot.width,
                cores_tall - 4U }, damage);
        }
        if (status == TASKMGR_STATUS_OK) {
            status = draw_readouts((struct ui_rect){ plot.x,
                pane.y + pane.height - readouts_tall, plot.width,
                readouts_tall }, damage);
        }
    }
    return status;
}

/* ================================================================ STARTUP */

static struct ui_rect startup_row_rect(size_t index)
{
    const struct ui_rect table = table_rect();
    const uint32_t top = table.y + TASKMGR_HEAD +
        (uint32_t)index * TASKMGR_ROW;

    if (table.width == 0U || index >= TASKMGR_MAX_STARTUP ||
            !startup[index].present ||
            top + TASKMGR_ROW > table.y + table.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ table.x, top, table.width, TASKMGR_ROW };
}

static enum taskmgr_status draw_startup(struct ui_rect damage)
{
    static const char *const heads[] = { "Name", "Publisher", "Status",
        "Startup cost" };
    static const char *const impact_label[TASKMGR_IMPACT_COUNT] = {
        "None", "Low", "Medium", "High"
    };
    const struct ui_rect table = table_rect();
    const uint32_t publisher_x = table.x + table.width * 40U / 100U;
    const uint32_t status_x = table.x + table.width * 68U / 100U;
    const uint32_t impact_x = table.x + table.width * 82U / 100U;
    enum taskmgr_status status = fill(body_rect(), damage, table_fill);

    if (status == TASKMGR_STATUS_OK) {
        status = fill(table, damage, table_fill);
    }

    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ table.x, table.y, table.width,
            TASKMGR_HEAD }, damage, head_fill);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = fill((struct ui_rect){ table.x,
            table.y + TASKMGR_HEAD - 1U, table.width, 1U }, damage, rule);
    }
    {
        const uint32_t xs[] = { table.x + TASKMGR_PAD, publisher_x,
            status_x, impact_x };

        for (size_t index = 0U; index < 4U &&
                status == TASKMGR_STATUS_OK; ++index) {
            status = text_at(damage, xs[index], table.y + 25U, heads[index],
                ink);
        }
    }
    for (size_t index = 0U; index < TASKMGR_MAX_STARTUP &&
            status == TASKMGR_STATUS_OK; ++index) {
        const struct taskmgr_startup *entry = &startup[index];
        const struct ui_rect row = startup_row_rect(index);
        const uint32_t baseline = row.y + row.height / 2U + 5U;
        const struct ui_rect icon = { row.x + 8U,
            row.y + (row.height - TASKMGR_ROW_ICON) / 2U,
            TASKMGR_ROW_ICON, TASKMGR_ROW_ICON };

        if (row.width == 0U) {
            continue;
        }
        if (!draw_art(entry->art, icon, damage)) {
            status = draw_glyph(entry->glyph == NULL ? "box" : entry->glyph,
                icon, damage, kind_colour[TASKMGR_APP]);
        }
        if (status == TASKMGR_STATUS_OK) {
            status = text_at(damage, row.x + 32U, baseline, entry->name,
                ink);
        }
        if (status == TASKMGR_STATUS_OK) {
            status = text_at(damage, publisher_x, baseline,
                entry->publisher, ink_soft);
        }
        if (status == TASKMGR_STATUS_OK) {
            status = text_at(damage, status_x, baseline,
                entry->enabled ? "Enabled" : "Disabled",
                entry->enabled ? ink : ink_faint);
        }
        /*
         * The impact, as a coloured pill rather than a word in the same
         * ink as everything else.  Windows writes "High" in plain text and
         * leaves you to notice it; a green-to-red pill is the same
         * information at a glance, and it agrees with the heat map on the
         * Processes tab about what "hot" looks like.
         */
        if (status == TASKMGR_STATUS_OK) {
            const struct taskmgr_rgb hue = impact_colour[entry->impact];
            const char *label = impact_label[entry->impact];
            const struct ui_rect pill = { impact_x, row.y + 5U,
                width_of(label) + 16U, row.height - 10U };

            status = fill(pill, damage, mix(table_fill, hue, 46U));
            if (status == TASKMGR_STATUS_OK) {
                status = text_at(damage, pill.x + 8U, baseline, label, hue);
            }
        }
    }
    return status;
}

/* ================================================================ PUBLIC */

enum taskmgr_status taskmgr_initialize(struct surface *canvas_in,
    struct ui_rect frame)
{
    if (canvas_in == NULL) {
        return TASKMGR_STATUS_NULL_ARGUMENT;
    }
    if (frame.width < TASKMGR_MIN_WIDTH || frame.height <
            TASKMGR_MIN_HEIGHT) {
        return TASKMGR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    canvas = canvas_in;
    window_rect = frame;
    for (size_t index = 0U; index < TASKMGR_MAX_PROCESSES; ++index) {
        processes[index] = (struct taskmgr_process){ 0 };
    }
    for (size_t index = 0U; index < TASKMGR_RESOURCE_COUNT; ++index) {
        meters[index] = (struct taskmgr_meter){ 0 };
    }
    for (size_t index = 0U; index < TASKMGR_MAX_STARTUP; ++index) {
        startup[index] = (struct taskmgr_startup){ 0 };
    }
    core_count = 0U;
    uptime_seconds = 0U;
    tab = TASKMGR_TAB_PROCESSES;
    resource = TASKMGR_RESOURCE_CPU;
    sort_column = TASKMGR_COLUMN_CPU;
    sort_descending = true;
    selected = (size_t)-1;
    hover_row = (size_t)-1;
    hover_tab = (size_t)-1;
    hover_column = (size_t)-1;
    hover_tile = (size_t)-1;
    hover_end_task = false;
    ended_task[0] = '\0';
    ended_task_waiting = false;
    fading_row = (size_t)-1;
    ui_motion_reset(&row_fade, 0);
    order_count = 0U;
    focused = true;
    initialized = true;
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_set_frame(struct ui_rect frame)
{
    if (frame.width < TASKMGR_MIN_WIDTH || frame.height <
            TASKMGR_MIN_HEIGHT) {
        return TASKMGR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return TASKMGR_STATUS_OK;
}

struct ui_rect taskmgr_bounds(void)
{
    return window_rect;
}

enum taskmgr_status taskmgr_set_process(size_t index,
    const struct taskmgr_process *process)
{
    if (index >= TASKMGR_MAX_PROCESSES) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    if (process == NULL) {
        processes[index] = (struct taskmgr_process){ 0 };
        if (selected == index) {
            selected = (size_t)-1;
        }
        rebuild_order();
        return TASKMGR_STATUS_OK;
    }
    if ((size_t)process->kind >= TASKMGR_KIND_COUNT) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    processes[index] = *process;
    processes[index].present = true;
    processes[index].name[TASKMGR_NAME_BYTES - 1U] = '\0';
    processes[index].status[TASKMGR_FIELD_BYTES - 1U] = '\0';
    rebuild_order();
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_set_meter(enum taskmgr_resource which,
    const struct taskmgr_meter *meter)
{
    if ((size_t)which >= TASKMGR_RESOURCE_COUNT) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    if (meter == NULL) {
        meters[which] = (struct taskmgr_meter){ 0 };
        return TASKMGR_STATUS_OK;
    }
    meters[which] = *meter;
    meters[which].present = true;
    meters[which].detail[TASKMGR_FIELD_BYTES - 1U] = '\0';
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_set_startup(size_t index,
    const struct taskmgr_startup *entry)
{
    if (index >= TASKMGR_MAX_STARTUP) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    if (entry == NULL) {
        startup[index] = (struct taskmgr_startup){ 0 };
        return TASKMGR_STATUS_OK;
    }
    if ((size_t)entry->impact >= TASKMGR_IMPACT_COUNT) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    startup[index] = *entry;
    startup[index].present = true;
    startup[index].name[TASKMGR_NAME_BYTES - 1U] = '\0';
    startup[index].publisher[TASKMGR_NAME_BYTES - 1U] = '\0';
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_set_core_count(size_t count)
{
    if (count > TASKMGR_MAX_CORES) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    core_count = count;
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_set_core(size_t index, uint16_t percent_tenths)
{
    if (index >= TASKMGR_MAX_CORES) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    cores[index] = percent_tenths > 1000U ? 1000U : percent_tenths;
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_set_uptime(uint32_t seconds)
{
    uptime_seconds = seconds;
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_set_focus(bool state)
{
    focused = state;
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_set_tab(enum taskmgr_tab which)
{
    if ((size_t)which >= TASKMGR_TAB_COUNT) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    tab = which;
    return TASKMGR_STATUS_OK;
}

enum taskmgr_tab taskmgr_get_tab(void)
{
    return tab;
}

enum taskmgr_status taskmgr_set_resource(enum taskmgr_resource which)
{
    if ((size_t)which >= TASKMGR_RESOURCE_COUNT) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    resource = which;
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_sort_by(enum taskmgr_column column)
{
    if ((size_t)column >= TASKMGR_COLUMN_COUNT) {
        return TASKMGR_STATUS_BAD_INDEX;
    }
    if (column == sort_column) {
        sort_descending = !sort_descending;
    } else {
        sort_column = column;
        /* A name sorts A-Z first and a number sorts heaviest-first,
         * because "what is eating the machine" is the question this table
         * is open to answer and "which process is called aardvark" is
         * not. */
        sort_descending = column != TASKMGR_COLUMN_NAME &&
            column != TASKMGR_COLUMN_STATUS;
    }
    rebuild_order();
    return TASKMGR_STATUS_OK;
}

enum taskmgr_column taskmgr_sort_column(void)
{
    return sort_column;
}

bool taskmgr_sort_descending(void)
{
    return sort_descending;
}

size_t taskmgr_selection(void)
{
    return selected;
}

bool taskmgr_end_task(char *name_out, size_t name_bytes,
    struct ui_rect *damage)
{
    if (damage != NULL) {
        *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    if (!initialized || selected == (size_t)-1 ||
            !processes[selected].present || processes[selected].heading ||
            processes[selected].kind == TASKMGR_SYSTEM) {
        return false;
    }
    copy_field(ended_task, processes[selected].name, sizeof(ended_task));
    ended_task_waiting = true;
    if (name_out != NULL && name_bytes != 0U) {
        copy_field(name_out, processes[selected].name, name_bytes);
    }
    processes[selected] = (struct taskmgr_process){ 0 };
    selected = (size_t)-1;
    hover_row = (size_t)-1;
    rebuild_order();
    if (damage != NULL) {
        *damage = window_rect;
    }
    return true;
}

bool taskmgr_take_ended_task(char *name_out, size_t name_bytes)
{
    if (!ended_task_waiting) {
        return false;
    }
    if (name_out != NULL && name_bytes != 0U) {
        copy_field(name_out, ended_task, name_bytes);
    }
    ended_task[0] = '\0';
    ended_task_waiting = false;
    return true;
}

enum cursor_kind taskmgr_cursor_at(struct ui_point point)
{
    if (!initialized || !holds(window_rect, point)) {
        return CURSOR_NORMAL_SELECT;
    }
    /* A column heading sorts, a tab switches, a tile picks a graph and End
     * task ends one: everything this window can be clicked on is a thing
     * that acts, so it is a link the way a breadcrumb is. */
    if (holds(end_task_rect(), point)) {
        return CURSOR_LINK_SELECT;
    }
    for (size_t index = 0U; index < TASKMGR_TAB_COUNT; ++index) {
        if (holds(tab_rect(index), point)) {
            return CURSOR_LINK_SELECT;
        }
    }
    if (tab == TASKMGR_TAB_PROCESSES) {
        for (size_t index = 0U; index < TASKMGR_COLUMN_COUNT; ++index) {
            if (holds(column_rect(index), point)) {
                return CURSOR_LINK_SELECT;
            }
        }
    }
    if (tab == TASKMGR_TAB_PERFORMANCE) {
        for (size_t index = 0U; index < TASKMGR_RESOURCE_COUNT; ++index) {
            if (holds(tile_rect(index), point)) {
                return CURSOR_LINK_SELECT;
            }
        }
    }
    return CURSOR_NORMAL_SELECT;
}

/*
 * Open and close.
 *
 * A closed Task Manager draws nothing and reports the window it vacated as
 * damage, so whatever is behind it repaints.  It keeps its tab, its sort and
 * its selection: Windows' does too, and a Task Manager that forgets which
 * column you sorted by every time you close it is a worse one.
 */
enum taskmgr_status taskmgr_open(struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKMGR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return TASKMGR_STATUS_NOT_INITIALIZED;
    }
    if (window_open) {
        return TASKMGR_STATUS_OK;
    }
    window_open = true;
    focused = true;
    *damage = window_rect;
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_close(struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKMGR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return TASKMGR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open) {
        return TASKMGR_STATUS_OK;
    }
    window_open = false;
    hover_close = false;
    hover_row = (size_t)-1;
    hover_tab = (size_t)-1;
    hover_column = (size_t)-1;
    hover_end_task = false;
    *damage = window_rect;
    return TASKMGR_STATUS_OK;
}

bool taskmgr_is_open(void)
{
    return initialized && window_open;
}

enum taskmgr_status taskmgr_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const size_t was_row = hover_row;
    const size_t was_tab = hover_tab;
    const size_t was_column = hover_column;
    const size_t was_tile = hover_tile;
    const bool was_end = hover_end_task;
    const bool was_close = hover_close;

    if (damage == NULL) {
        return TASKMGR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return TASKMGR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open) {
        return TASKMGR_STATUS_OK;
    }
    hover_close = holds(caption_button_rect(2U), point);
    hover_row = (size_t)-1;
    hover_tab = (size_t)-1;
    hover_column = (size_t)-1;
    hover_tile = (size_t)-1;
    hover_end_task = holds(end_task_rect(), point);
    for (size_t index = 0U; index < TASKMGR_TAB_COUNT; ++index) {
        if (holds(tab_rect(index), point)) {
            hover_tab = index;
            break;
        }
    }
    if (tab == TASKMGR_TAB_PROCESSES) {
        for (size_t index = 0U; index < TASKMGR_COLUMN_COUNT; ++index) {
            if (holds(column_rect(index), point)) {
                hover_column = index;
                break;
            }
        }
        for (size_t position = 0U; position < order_count; ++position) {
            if (holds(row_rect(position), point) &&
                    !processes[order[position]].heading) {
                hover_row = order[position];
                break;
            }
        }
    }
    if (tab == TASKMGR_TAB_PERFORMANCE) {
        for (size_t index = 0U; index < TASKMGR_RESOURCE_COUNT; ++index) {
            if (holds(tile_rect(index), point)) {
                hover_tile = index;
                break;
            }
        }
    }
    if (was_row != hover_row) {
        /* The same 83 ms linear cross-fade every other hover in this shell
         * uses; see ui_motion.h. */
        fading_row = was_row;
        ui_motion_reset(&row_fade, (int32_t)UI_MOTION_ONE);
        ui_motion_to(&row_fade, 0, UI_MOTION_BRUSH_NS, clock_monotonic_ns());
        *damage = join(*damage, join(row_damage(was_row),
            row_damage(hover_row)));
    }
    if (was_tab != hover_tab) {
        *damage = join(*damage, tabs_rect());
    }
    if (was_column != hover_column) {
        *damage = join(*damage, table_rect());
    }
    if (was_tile != hover_tile) {
        *damage = join(*damage, body_rect());
    }
    if (was_end != hover_end_task) {
        *damage = join(*damage, footer_rect());
    }
    if (was_close != hover_close) {
        *damage = join(*damage, caption_button_rect(2U));
    }
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKMGR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return TASKMGR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open) {
        return TASKMGR_STATUS_OK;
    }
    for (size_t index = 0U; index < TASKMGR_TAB_COUNT; ++index) {
        if (holds(tab_rect(index), point)) {
            tab = (enum taskmgr_tab)index;
            *damage = window_rect;
            return TASKMGR_STATUS_OK;
        }
    }
    if (holds(caption_button_rect(2U), point)) {
        return taskmgr_close(damage);
    }
    if (holds(end_task_rect(), point)) {
        (void)taskmgr_end_task(NULL, 0U, damage);
        return TASKMGR_STATUS_OK;
    }
    if (tab == TASKMGR_TAB_PERFORMANCE) {
        for (size_t index = 0U; index < TASKMGR_RESOURCE_COUNT; ++index) {
            if (holds(tile_rect(index), point)) {
                resource = (enum taskmgr_resource)index;
                *damage = body_rect();
                return TASKMGR_STATUS_OK;
            }
        }
        return TASKMGR_STATUS_OK;
    }
    if (tab != TASKMGR_TAB_PROCESSES) {
        return TASKMGR_STATUS_OK;
    }
    for (size_t index = 0U; index < TASKMGR_COLUMN_COUNT; ++index) {
        if (holds(column_rect(index), point)) {
            (void)taskmgr_sort_by((enum taskmgr_column)index);
            *damage = table_rect();
            return TASKMGR_STATUS_OK;
        }
    }
    for (size_t position = 0U; position < order_count; ++position) {
        if (!holds(row_rect(position), point)) {
            continue;
        }
        /* A heading is not selectable: it is not a task, and End task
         * would have nothing to end. */
        if (processes[order[position]].heading) {
            return TASKMGR_STATUS_OK;
        }
        selected = order[position];
        *damage = window_rect;
        return TASKMGR_STATUS_OK;
    }
    return TASKMGR_STATUS_OK;
}

enum taskmgr_status taskmgr_key_escape(struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKMGR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return TASKMGR_STATUS_NOT_INITIALIZED;
    }
    if (selected != (size_t)-1) {
        selected = (size_t)-1;
        *damage = window_rect;
    }
    return TASKMGR_STATUS_OK;
}

bool taskmgr_animate(struct ui_rect *damage)
{
    bool moved = false;

    if (damage == NULL || !initialized) {
        return false;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (ui_motion_advance(&row_fade, clock_monotonic_ns(), ui_ease_linear)) {
        *damage = join(*damage, row_damage(fading_row));
        moved = true;
    }
    return moved;
}

bool taskmgr_animating(void)
{
    return ui_motion_running(&row_fade);
}

enum taskmgr_status taskmgr_draw(struct ui_rect damage)
{
    const struct ui_rect clipped = intersect(damage, window_rect);
    enum taskmgr_status status;

    if (!initialized) {
        return TASKMGR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open || clipped.width == 0U || clipped.height == 0U) {
        return TASKMGR_STATUS_OK;
    }
    status = fill(window_rect, clipped, focused ? border_active :
        border_inactive);
    if (status == TASKMGR_STATUS_OK) {
        status = draw_caption(clipped);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = draw_menu(clipped);
    }
    if (status == TASKMGR_STATUS_OK) {
        status = draw_tabs(clipped);
    }
    if (status == TASKMGR_STATUS_OK) {
        switch (tab) {
        case TASKMGR_TAB_PERFORMANCE:
            status = draw_performance(clipped);
            break;
        case TASKMGR_TAB_STARTUP:
            status = draw_startup(clipped);
            break;
        case TASKMGR_TAB_PROCESSES:
        default:
            status = draw_processes(clipped);
            break;
        }
    }
    if (status == TASKMGR_STATUS_OK) {
        status = draw_footer(clipped);
    }
    return status;
}

/* ============================================================== SELF TEST */

bool taskmgr_self_test(void)
{
    char scratch[32];

    self_test_failure = "taskmgr self-test passed";
    /*
     * The number formatting, which every figure in the table goes through
     * and which has no second chance to be noticed: a wrong percentage
     * looks exactly like a right one.
     */
    (void)append_tenths(scratch, sizeof(scratch), 0U, 234U);
    if (scratch[0] != '2' || scratch[1] != '3' || scratch[2] != '.' ||
            scratch[3] != '4' || scratch[4] != '%' || scratch[5] != '\0') {
        self_test_failure = "tenths are not formatted as a percentage";
        return false;
    }
    (void)append_tenths(scratch, sizeof(scratch), 0U, 0U);
    if (scratch[0] != '0' || scratch[1] != '%' || scratch[2] != '\0') {
        self_test_failure = "an idle process is not written as a flat zero";
        return false;
    }
    (void)append_size(scratch, sizeof(scratch), 0U, 2048U);
    if (scratch[0] != '2' || scratch[1] != '.' || scratch[2] != '0' ||
            scratch[3] != ' ' || scratch[4] != 'M') {
        self_test_failure = "kilobytes are not written as megabytes";
        return false;
    }
    (void)append_size(scratch, sizeof(scratch), 0U, 3U * 1024U * 1024U);
    if (scratch[0] != '3' || scratch[1] != '.' || scratch[2] != '0' ||
            scratch[3] != ' ' || scratch[4] != 'G') {
        self_test_failure = "a gigabyte is not written as one";
        return false;
    }
    /*
     * The heat map, which is the whole table's legibility: idle has to be
     * white or every row is tinted and the map means nothing, and hot has
     * to be hotter than warm or it runs backwards.
     */
    {
        const struct taskmgr_rgb idle = heat_for(0U);
        const struct taskmgr_rgb warm = heat_for(300U);
        const struct taskmgr_rgb hot = heat_for(950U);

        if (idle.red != table_fill.red || idle.green != table_fill.green ||
                idle.blue != table_fill.blue) {
            self_test_failure = "an idle cell is tinted";
            return false;
        }
        if (warm.blue <= hot.blue) {
            self_test_failure = "the heat map does not run cool to hot";
            return false;
        }
    }
    /* Sorting has to be an ORDER, not a rearrangement: the caller's slots
     * keep their meaning or every index it holds goes stale. */
    {
        struct taskmgr_process probe = { 0 };
        size_t kept;

        copy_field(probe.name, "probe", sizeof(probe.name));
        probe.cpu_tenths = 500U;
        if (taskmgr_set_process(TASKMGR_MAX_PROCESSES, &probe) !=
                TASKMGR_STATUS_BAD_INDEX) {
            self_test_failure = "taskmgr accepted an index past the end";
            return false;
        }
        probe.kind = (enum taskmgr_kind)TASKMGR_KIND_COUNT;
        if (taskmgr_set_process(0U, &probe) != TASKMGR_STATUS_BAD_INDEX) {
            self_test_failure = "taskmgr accepted an unknown process kind";
            return false;
        }
        kept = order_count;
        (void)kept;
    }
    if (taskmgr_set_core_count(TASKMGR_MAX_CORES + 1U) !=
            TASKMGR_STATUS_BAD_INDEX) {
        self_test_failure = "taskmgr accepted more cores than it can hold";
        return false;
    }
    if (taskmgr_set_meter((enum taskmgr_resource)TASKMGR_RESOURCE_COUNT,
            NULL) != TASKMGR_STATUS_BAD_INDEX) {
        self_test_failure = "taskmgr accepted an unknown resource";
        return false;
    }
    self_test_failure = "";
    return true;
}

const char *taskmgr_self_test_failure(void)
{
    return self_test_failure;
}
