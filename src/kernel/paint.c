/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Paint.  See include/phipia/paint.h for the shape of the ribbon and for
 * what Phipia does differently.
 */

#include <phipia/paint.h>

#include <phipia/cursor.h>

#include <phipia/framebuffer.h>
#include <phipia/heap.h>
#include <phipia/ui_font.h>

#include "paint_glyphs.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION: read off a Windows 10 Paint window at 100% scaling.
 * The GROUPS and their order are Paint's; the pixel sizes are this file's
 * reading of them.
 */

#define PAINT_BORDER 1U
#define PAINT_CAPTION 30U
#define PAINT_CAPTION_BUTTON 46U
#define PAINT_QAT_SLOT 22U
#define PAINT_TABS 26U
#define PAINT_TAB_PAD 14U
#define PAINT_RIBBON 100U
#define PAINT_RIBBON_LABEL 15U
#define PAINT_STATUS 26U
#define PAINT_PAD 6U
#define PAINT_GROUP_GAP 7U

/* A tall ribbon button: a mark over one or two lines of label. */
#define PAINT_TALL_WIDTH 54U
#define PAINT_TALL_ICON 24U
/* A small one, in a stacked column. */
#define PAINT_SMALL_HEIGHT 21U
#define PAINT_SMALL_ICON 16U
/*
 * The smallest box a Lucide mark may be asked for.  The set is rasterized
 * at sixteen, twenty, twenty-four and thirty-two; asking for twelve does
 * not produce a twelve-pixel mark, it produces the sixteen drawn outside
 * the box, because nothing here resamples a glyph to fit one.
 */
#define PAINT_MARK 16U
/* The tool grid, which is three across and two down and never a row. */
#define PAINT_TOOL_CELL 26U
#define PAINT_TOOL_COLUMNS 3U
#define PAINT_TOOL_ROWS 2U
/* The shapes gallery: three rows of seven, with its scroll arrows beside. */
#define PAINT_SHAPE_CELL 22U
#define PAINT_SHAPE_COLUMNS 7U
#define PAINT_SHAPE_ROWS 3U
#define PAINT_GALLERY_ARROWS 16U   /* the smallest mark there is */
/* The colour palette: two rows of ten, and the two big colour buttons. */
#define PAINT_SWATCH 15U
#define PAINT_SWATCH_COLUMNS 10U
#define PAINT_COLOUR_BUTTON 74U
/* The canvas, and the handles Paint puts on its right and bottom edges. */
#define PAINT_SHEET_MARGIN 12U
#define PAINT_HANDLE 5U
/* The zoom slider at the right of the status bar. */
#define PAINT_ZOOM_TRACK 92U
#define PAINT_ZOOM_THUMB 7U
#define PAINT_CANVAS_WIDTH 1024U
#define PAINT_CANVAS_HEIGHT 768U
#define PAINT_TEXT_BYTES 96U
#define PAINT_CLIPBOARD_WIDTH 320U
#define PAINT_CLIPBOARD_HEIGHT 240U
#define PAINT_RESIZE_DIALOG_WIDTH 340U
#define PAINT_RESIZE_DIALOG_HEIGHT 214U
#define PAINT_CLIPBOARD_PIXELS \
    (PAINT_CLIPBOARD_WIDTH * PAINT_CLIPBOARD_HEIGHT)
#define PAINT_CANVAS_PIXELS (PAINT_CANVAS_WIDTH * PAINT_CANVAS_HEIGHT)
#define PAINT_CANVAS_BYTES \
    ((uint64_t)PAINT_CANVAS_PIXELS * sizeof(uint32_t))
#define PAINT_CLIPBOARD_BYTES \
    ((uint64_t)PAINT_CLIPBOARD_PIXELS * sizeof(uint32_t))

/* ================================================================ PALETTE
 *
 * Paint's own twenty default swatches, in the order it lays them out: ten
 * across the top row and ten under them.  These are not chosen - they are
 * the values Paint has shipped with since the ribbon arrived, and the names
 * beside them are Paint's own.
 */
struct paint_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define PAINT_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct paint_rgb swatches[PAINT_SWATCHES] = {
    PAINT_RGB(0x00U, 0x00U, 0x00U),   /* Black          */
    PAINT_RGB(0x7FU, 0x7FU, 0x7FU),   /* Grey-50%       */
    PAINT_RGB(0x88U, 0x00U, 0x15U),   /* Dark red       */
    PAINT_RGB(0xEDU, 0x1CU, 0x24U),   /* Red            */
    PAINT_RGB(0xFFU, 0x7FU, 0x27U),   /* Orange         */
    PAINT_RGB(0xFFU, 0xF2U, 0x00U),   /* Yellow         */
    PAINT_RGB(0x22U, 0xB1U, 0x4CU),   /* Green          */
    PAINT_RGB(0x00U, 0xA2U, 0xE8U),   /* Turquoise      */
    PAINT_RGB(0x3FU, 0x48U, 0xCCU),   /* Indigo         */
    PAINT_RGB(0xA3U, 0x49U, 0xA4U),   /* Purple         */
    PAINT_RGB(0xFFU, 0xFFU, 0xFFU),   /* White          */
    PAINT_RGB(0xC3U, 0xC3U, 0xC3U),   /* Grey-25%       */
    PAINT_RGB(0xB9U, 0x7AU, 0x57U),   /* Brown          */
    PAINT_RGB(0xFFU, 0xAEU, 0xC9U),   /* Rose           */
    PAINT_RGB(0xFFU, 0xC9U, 0x0EU),   /* Gold           */
    PAINT_RGB(0xEFU, 0xE4U, 0xB0U),   /* Light yellow   */
    PAINT_RGB(0xB5U, 0xE6U, 0x1DU),   /* Lime           */
    PAINT_RGB(0x99U, 0xD9U, 0xEAU),   /* Light turquoise*/
    PAINT_RGB(0x70U, 0x92U, 0xBEU),   /* Blue-grey      */
    PAINT_RGB(0xC8U, 0xBFU, 0xE7U)    /* Lavender       */
};

/* The chrome, which is Windows' rather than Paint's. */
static const struct paint_rgb caption_fill = PAINT_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct paint_rgb chrome = PAINT_RGB(0xF0U, 0xF0U, 0xF0U);
static const struct paint_rgb ribbon_fill = PAINT_RGB(0xF7U, 0xF7U, 0xF7U);
static const struct paint_rgb rule = PAINT_RGB(0xDCU, 0xDCU, 0xDCU);
static const struct paint_rgb rule_soft = PAINT_RGB(0xE8U, 0xE8U, 0xE8U);
static const struct paint_rgb ink = PAINT_RGB(0x1AU, 0x1AU, 0x1AU);
static const struct paint_rgb ink_soft = PAINT_RGB(0x5AU, 0x5AU, 0x5AU);
static const struct paint_rgb ink_faint = PAINT_RGB(0x8CU, 0x8CU, 0x8CU);
static const struct paint_rgb accent = PAINT_RGB(0x00U, 0x78U, 0xD7U);
static const struct paint_rgb on_accent = PAINT_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct paint_rgb hover_fill = PAINT_RGB(0xE8U, 0xF1U, 0xFAU);
/* Paint's workspace: the grey the sheet floats on. */
static const struct paint_rgb workspace = PAINT_RGB(0x9EU, 0x9EU, 0x9EU);
static const struct paint_rgb sheet = PAINT_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct paint_rgb sheet_edge = PAINT_RGB(0x60U, 0x60U, 0x60U);
static const struct paint_rgb border_active = PAINT_RGB(0x00U, 0x78U, 0xD7U);
static const struct paint_rgb border_inactive = PAINT_RGB(0x9BU, 0x9BU,
    0x9BU);

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool focused = true;
static enum paint_tool current_tool = PAINT_TOOL_PENCIL;
static size_t current_shape;
static size_t colour_one;          /* index into swatches */
static size_t colour_two = 10U;    /* White, which is Paint's default */
static uint32_t zoom_percent = 100U;
static uint32_t canvas_pixels[PAINT_CANVAS_PIXELS];
static uint32_t *undo_pixels;
static uint32_t *redo_pixels;
static uint32_t *clipboard_pixels;
static uint32_t image_width = PAINT_CANVAS_WIDTH;
static uint32_t image_height = PAINT_CANVAS_HEIGHT;
static uint32_t undo_width;
static uint32_t undo_height;
static uint32_t redo_width;
static uint32_t redo_height;
static uint32_t clipboard_width;
static uint32_t clipboard_height;
static bool undo_valid;
static bool redo_valid;
static bool clipboard_valid;
static bool image_dirty;
static bool save_requested;
static bool painting;
static struct ui_point last_paint_point;
static uint32_t stroke_radius = 1U;
static bool selecting;
static bool select_mode;
static bool selection_valid;
static struct ui_point selection_origin;
static struct ui_rect selection;
static bool shape_mode;
static bool shape_dragging;
static struct ui_point shape_origin;
static bool shape_outline = true;
static bool shape_fill;
static size_t colour_target;
static bool text_active;
static struct ui_point text_origin;
static char text_buffer[PAINT_TEXT_BYTES];
static size_t text_length;
static bool resize_dialog_open;
static bool resize_keep_aspect = true;
static size_t resize_field;
static uint32_t resize_width_value = PAINT_CANVAS_WIDTH;
static uint32_t resize_height_value = PAINT_CANVAS_HEIGHT;
static char window_title[48] = "Untitled - Paint";
static size_t hover_tool = (size_t)-1;
static size_t hover_shape = (size_t)-1;
static size_t hover_swatch = (size_t)-1;
static const char *self_test_failure = "paint self-test has not run";

const char *paint_status_string(enum paint_status status)
{
    switch (status) {
    case PAINT_STATUS_OK:
        return "ok";
    case PAINT_STATUS_NULL_ARGUMENT:
        return "null argument";
    case PAINT_STATUS_NOT_INITIALIZED:
        return "paint not initialized";
    case PAINT_STATUS_BAD_INDEX:
        return "paint index is out of range";
    case PAINT_STATUS_UNSUPPORTED_GEOMETRY:
        return "paint geometry is unsupported";
    case PAINT_STATUS_SURFACE_FAILURE:
        return "paint surface refused a pixel";
    case PAINT_STATUS_ALLOCATION_FAILURE:
        return "paint editing buffers could not be allocated";
    default:
        return "unknown paint status";
    }
}

/* ============================================================== PRIMITIVES */

static bool allocate_editing_buffers(void)
{
    if (undo_pixels != NULL && redo_pixels != NULL &&
            clipboard_pixels != NULL) {
        return true;
    }

    if (undo_pixels != NULL) {
        (void)heap_free(undo_pixels);
        undo_pixels = NULL;
    }
    if (redo_pixels != NULL) {
        (void)heap_free(redo_pixels);
        redo_pixels = NULL;
    }
    if (clipboard_pixels != NULL) {
        (void)heap_free(clipboard_pixels);
        clipboard_pixels = NULL;
    }

    if (heap_allocate(PAINT_CANVAS_BYTES, (void **)&undo_pixels) !=
            HEAP_STATUS_OK) {
        return false;
    }
    if (heap_allocate(PAINT_CANVAS_BYTES, (void **)&redo_pixels) !=
            HEAP_STATUS_OK) {
        (void)heap_free(undo_pixels);
        undo_pixels = NULL;
        return false;
    }
    if (heap_allocate(PAINT_CLIPBOARD_BYTES, (void **)&clipboard_pixels) !=
            HEAP_STATUS_OK) {
        (void)heap_free(redo_pixels);
        (void)heap_free(undo_pixels);
        redo_pixels = NULL;
        undo_pixels = NULL;
        return false;
    }
    return true;
}

static uint32_t pack_rgb(struct paint_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return ((uint32_t)colour.red << format.red_position) |
        ((uint32_t)colour.green << format.green_position) |
        ((uint32_t)colour.blue << format.blue_position);
}

static void copy_canvas(uint32_t *destination, const uint32_t *source)
{
    for (size_t index = 0U;
         index < (size_t)PAINT_CANVAS_WIDTH * PAINT_CANVAS_HEIGHT; ++index) {
        destination[index] = source[index];
    }
}

static void capture_undo(void)
{
    copy_canvas(undo_pixels, canvas_pixels);
    undo_width = image_width;
    undo_height = image_height;
    undo_valid = true;
    redo_valid = false;
}

static void undo_image(void)
{
    if (!undo_valid) {
        return;
    }
    copy_canvas(redo_pixels, canvas_pixels);
    redo_width = image_width;
    redo_height = image_height;
    redo_valid = true;
    copy_canvas(canvas_pixels, undo_pixels);
    image_width = undo_width;
    image_height = undo_height;
    undo_valid = false;
    selection_valid = false;
    text_active = false;
    image_dirty = true;
}

static void redo_image(void)
{
    if (!redo_valid) {
        return;
    }
    copy_canvas(undo_pixels, canvas_pixels);
    undo_width = image_width;
    undo_height = image_height;
    undo_valid = true;
    copy_canvas(canvas_pixels, redo_pixels);
    image_width = redo_width;
    image_height = redo_height;
    redo_valid = false;
    selection_valid = false;
    text_active = false;
    image_dirty = true;
}

static struct ui_rect intersect(struct ui_rect left, struct ui_rect right)
{
    const uint32_t x0 = left.x > right.x ? left.x : right.x;
    const uint32_t y0 = left.y > right.y ? left.y : right.y;
    const uint32_t x1a = left.x + left.width;
    const uint32_t x1b = right.x + right.width;
    const uint32_t y1a = left.y + left.height;
    const uint32_t y1b = right.y + right.height;
    const uint32_t x1 = x1a < x1b ? x1a : x1b;
    const uint32_t y1 = y1a < y1b ? y1a : y1b;

    if (x1 <= x0 || y1 <= y0) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ x0, y0, x1 - x0, y1 - y0 };
}

static bool holds(struct ui_rect area, struct ui_point point)
{
    return point.x >= 0 && point.y >= 0 &&
        (uint32_t)point.x >= area.x &&
        (uint32_t)point.x < area.x + area.width &&
        (uint32_t)point.y >= area.y &&
        (uint32_t)point.y < area.y + area.height;
}

static enum paint_status fill(struct ui_rect area, struct ui_rect damage,
    struct paint_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return PAINT_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return PAINT_STATUS_OK;
}

static enum paint_status outline(struct ui_rect area, struct ui_rect damage,
    struct paint_rgb colour)
{
    enum paint_status status = fill((struct ui_rect){ area.x, area.y,
        area.width, 1U }, damage, colour);

    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y + area.height - 1U,
            area.width, 1U }, damage, colour);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y, 1U, area.height },
            damage, colour);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ area.x + area.width - 1U, area.y, 1U,
            area.height }, damage, colour);
    }
    return status;
}

static enum paint_status text_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct paint_rgb colour)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (clip.width == 0U || clip.height == 0U || body == NULL ||
            body[0] == '\0') {
        return PAINT_STATUS_OK;
    }
    bounds.x = window_rect.x;
    bounds.y = window_rect.y;
    bounds.width = window_rect.width;
    bounds.height = window_rect.height;
    region.x = clip.x;
    region.y = clip.y;
    region.width = clip.width;
    region.height = clip.height;
    (void)ui_font_draw_text_clipped(canvas, bounds, region, x, baseline, body,
        pack_rgb(colour), NULL);
    return PAINT_STATUS_OK;
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

static uint32_t centred_x(uint32_t left, uint32_t width, uint32_t text)
{
    return width > text ? left + (width - text) / 2U : left;
}

static bool names_match(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static const uint8_t *glyph_cell(const char *name, uint32_t wanted,
    uint32_t *size)
{
    for (size_t index = 0U; index < PAINT_LUCIDE_COUNT; ++index) {
        size_t choice = 0U;

        if (!names_match(paint_lucide[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < PAINT_LUCIDE_SIZES; ++option) {
            if (paint_lucide_size[option] <= wanted) {
                choice = option;
            }
        }
        *size = paint_lucide_size[choice];
        return paint_lucide[index].alpha[choice];
    }
    return NULL;
}

static enum paint_status draw_glyph(const char *name, struct ui_rect box,
    struct ui_rect damage, struct paint_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t wanted = box.width < box.height ? box.width : box.height;
    const uint32_t over = pack_rgb(colour);
    uint32_t size = 0U;
    const uint8_t *cell = glyph_cell(name, wanted, &size);
    struct ui_rect placed;
    struct ui_rect clipped;

    if (cell == NULL) {
        return PAINT_STATUS_OK;
    }
    placed = (struct ui_rect){
        box.x + (box.width > size ? (box.width - size) / 2U : 0U),
        box.y + (box.height > size ? (box.height - size) / 2U : 0U),
        size, size };
    clipped = intersect(placed, damage);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint8_t coverage = cell[local_y * size +
                (clipped.x - placed.x + x)];
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (coverage == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return PAINT_STATUS_SURFACE_FAILURE;
            }
            red = (((over >> format.red_position) & 0xFFU) * coverage +
                ((under >> format.red_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            green = (((over >> format.green_position) & 0xFFU) * coverage +
                ((under >> format.green_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            blue = (((over >> format.blue_position) & 0xFFU) * coverage +
                ((under >> format.blue_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) !=
                        SURFACE_STATUS_OK) {
                return PAINT_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return PAINT_STATUS_OK;
}

/* Two hex digits of a byte, which is what the colour buttons print. */
static void hex_byte(char *out, uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    out[0] = digits[(value >> 4) & 0x0FU];
    out[1] = digits[value & 0x0FU];
}

static void hex_colour(char *out, struct paint_rgb colour)
{
    out[0] = '#';
    hex_byte(out + 1, colour.red);
    hex_byte(out + 3, colour.green);
    hex_byte(out + 5, colour.blue);
    out[7] = '\0';
}

/* ================================================================ GEOMETRY */

static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){ window_rect.x + PAINT_BORDER,
        window_rect.y + PAINT_BORDER,
        window_rect.width - PAINT_BORDER * 2U, PAINT_CAPTION };
}

static struct ui_rect tabs_rect(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x, caption.y + caption.height,
        caption.width, PAINT_TABS };
}

static struct ui_rect ribbon_rect(void)
{
    const struct ui_rect tabs = tabs_rect();

    return (struct ui_rect){ tabs.x, tabs.y + tabs.height, tabs.width,
        PAINT_RIBBON };
}

static struct ui_rect status_rect(void)
{
    return (struct ui_rect){ window_rect.x + PAINT_BORDER,
        window_rect.y + window_rect.height - PAINT_BORDER - PAINT_STATUS,
        window_rect.width - PAINT_BORDER * 2U, PAINT_STATUS };
}

static struct ui_rect workspace_rect(void)
{
    const struct ui_rect band = ribbon_rect();
    const struct ui_rect bar = status_rect();

    return (struct ui_rect){ band.x, band.y + band.height, band.width,
        bar.y - (band.y + band.height) };
}

struct ui_rect paint_sheet_bounds(void)
{
    const struct ui_rect area = workspace_rect();
    /* Paint pins the sheet to the top-left of its workspace and grows it to
     * the right and down, which is why the handles are only on those edges. */
    const uint32_t width = area.width > PAINT_SHEET_MARGIN * 2U + 200U ?
        area.width - PAINT_SHEET_MARGIN * 2U - 120U : 200U;
    const uint32_t height = area.height > PAINT_SHEET_MARGIN * 2U + 120U ?
        area.height - PAINT_SHEET_MARGIN * 2U - 40U : 120U;

    return (struct ui_rect){ area.x + PAINT_SHEET_MARGIN,
        area.y + PAINT_SHEET_MARGIN, width, height };
}

/*
 * What the pointer means over Paint.
 *
 * The sheet takes a crosshair, which is what every raster editor since the
 * first one has put over a canvas and what Windows calls Precision Select:
 * an arrow has a hotspot in its corner and you cannot see the pixel you are
 * about to set.  The ribbon and the chrome are ordinary buttons and take
 * the ordinary arrow.
 *
 * Nothing here decides WHEN this is right - the compositor asks the window
 * under the pointer and this is Paint's answer.  See phipia/cursor.h.
 */
enum cursor_kind paint_cursor_at(struct ui_point point)
{
    if (!initialized || !holds(window_rect, point)) {
        return CURSOR_NORMAL_SELECT;
    }
    if (holds(paint_sheet_bounds(), point)) {
        return CURSOR_PRECISION_SELECT;
    }
    return CURSOR_NORMAL_SELECT;
}

struct ui_rect paint_bounds(void)
{
    return window_rect;
}

static struct ui_rect caption_button_rect(uint32_t index)
{
    const struct ui_rect caption = caption_rect();
    const uint32_t from_right = 3U - index;

    return (struct ui_rect){
        caption.x + caption.width - from_right * PAINT_CAPTION_BUTTON,
        caption.y, PAINT_CAPTION_BUTTON, caption.height };
}

/* --- the ribbon's seven groups, measured left to right --- */

/*
 * Each group's width is stated where the group is drawn, and both the
 * drawing and the hit testing walk the same list, so a group can never be
 * drawn in one place and clicked in another.
 */
enum paint_group {
    PAINT_GROUP_CLIPBOARD = 0,
    PAINT_GROUP_IMAGE,
    PAINT_GROUP_TOOLS,
    PAINT_GROUP_BRUSHES,
    PAINT_GROUP_SHAPES,
    PAINT_GROUP_SIZE,
    PAINT_GROUP_COLORS,
    PAINT_GROUP_COUNT
};

static uint32_t group_width(enum paint_group group)
{
    switch (group) {
    case PAINT_GROUP_CLIPBOARD:
        return PAINT_TALL_WIDTH + 66U;
    case PAINT_GROUP_IMAGE:
        return PAINT_TALL_WIDTH + 70U;
    case PAINT_GROUP_TOOLS:
        return PAINT_TOOL_CELL * PAINT_TOOL_COLUMNS + 8U;
    case PAINT_GROUP_BRUSHES:
        /* Wide enough for the word beneath it rather than for the button
         * alone: "Brushes" is wider than a tall button, so a group sized
         * to the button pushed its own label over the divider beside it.
         * paint_self_test() checks every group against its label. */
        return PAINT_TALL_WIDTH + 10U;
    case PAINT_GROUP_SHAPES:
        return PAINT_SHAPE_CELL * PAINT_SHAPE_COLUMNS +
            PAINT_GALLERY_ARROWS + 78U;
    case PAINT_GROUP_SIZE:
        return PAINT_TALL_WIDTH;
    case PAINT_GROUP_COLORS:
    default:
        return PAINT_COLOUR_BUTTON * 2U + 10U +
            PAINT_SWATCH * PAINT_SWATCH_COLUMNS + 10U + 58U;
    }
}

/* The word Paint prints under each group, and what each group has to be
 * wide enough to hold. */
static const char *group_label(enum paint_group group)
{
    switch (group) {
    case PAINT_GROUP_CLIPBOARD:
        return "Clipboard";
    case PAINT_GROUP_IMAGE:
        return "Image";
    case PAINT_GROUP_TOOLS:
        return "Tools";
    case PAINT_GROUP_BRUSHES:
        return "Brushes";
    case PAINT_GROUP_SHAPES:
        return "Shapes";
    case PAINT_GROUP_SIZE:
        return "Size";
    case PAINT_GROUP_COLORS:
    default:
        return "Colors";
    }
}

static uint32_t group_left(enum paint_group group)
{
    const struct ui_rect band = ribbon_rect();
    uint32_t pen = band.x + PAINT_PAD;

    for (enum paint_group scan = PAINT_GROUP_CLIPBOARD; scan < group;
         scan = (enum paint_group)(scan + 1)) {
        pen += group_width(scan) + PAINT_GROUP_GAP;
    }
    return pen;
}

static struct ui_rect tool_rect(size_t tool)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_TOOLS) + 4U;
    const uint32_t column = (uint32_t)tool % PAINT_TOOL_COLUMNS;
    const uint32_t row = (uint32_t)tool / PAINT_TOOL_COLUMNS;

    return (struct ui_rect){ left + column * PAINT_TOOL_CELL,
        band.y + 12U + row * PAINT_TOOL_CELL,
        PAINT_TOOL_CELL, PAINT_TOOL_CELL };
}

static struct ui_rect shape_rect(size_t shape)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_SHAPES) + 2U;
    const uint32_t column = (uint32_t)shape % PAINT_SHAPE_COLUMNS;
    const uint32_t row = (uint32_t)shape / PAINT_SHAPE_COLUMNS;

    return (struct ui_rect){ left + column * PAINT_SHAPE_CELL,
        band.y + 8U + row * PAINT_SHAPE_CELL,
        PAINT_SHAPE_CELL, PAINT_SHAPE_CELL };
}

static struct ui_rect swatch_rect(size_t index)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_COLORS) +
        PAINT_COLOUR_BUTTON * 2U + 10U;
    const uint32_t column = (uint32_t)index % PAINT_SWATCH_COLUMNS;
    const uint32_t row = (uint32_t)index / PAINT_SWATCH_COLUMNS;

    return (struct ui_rect){ left + column * PAINT_SWATCH,
        band.y + 20U + row * PAINT_SWATCH, PAINT_SWATCH, PAINT_SWATCH };
}

static struct ui_rect quick_rect(size_t index)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x + 6U + PAINT_QAT_SLOT + 2U +
        (uint32_t)index * PAINT_QAT_SLOT, caption.y, PAINT_QAT_SLOT,
        caption.height };
}

static struct ui_rect clipboard_rect(size_t item)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_CLIPBOARD);

    if (item == 0U) {
        return (struct ui_rect){ left, band.y + 4U, PAINT_TALL_WIDTH, 72U };
    }
    return (struct ui_rect){ left + PAINT_TALL_WIDTH + 4U,
        band.y + 14U + (uint32_t)(item - 1U) * PAINT_SMALL_HEIGHT,
        60U, PAINT_SMALL_HEIGHT };
}

static struct ui_rect image_rect(size_t item)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_IMAGE);

    if (item == 0U) {
        return (struct ui_rect){ left, band.y + 4U, PAINT_TALL_WIDTH, 72U };
    }
    return (struct ui_rect){ left + PAINT_TALL_WIDTH + 4U,
        band.y + 8U + (uint32_t)(item - 1U) * PAINT_SMALL_HEIGHT,
        64U, PAINT_SMALL_HEIGHT };
}

static struct ui_rect brushes_rect(void)
{
    const struct ui_rect band = ribbon_rect();

    return (struct ui_rect){ centred_x(group_left(PAINT_GROUP_BRUSHES),
        group_width(PAINT_GROUP_BRUSHES), PAINT_TALL_WIDTH), band.y + 4U,
        PAINT_TALL_WIDTH, 72U };
}

static struct ui_rect shape_option_rect(size_t item)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t gallery_right = group_left(PAINT_GROUP_SHAPES) + 2U +
        PAINT_SHAPE_CELL * PAINT_SHAPE_COLUMNS;

    return (struct ui_rect){ gallery_right + PAINT_GALLERY_ARROWS + 6U,
        band.y + 12U + (uint32_t)item * PAINT_SMALL_HEIGHT, 70U,
        PAINT_SMALL_HEIGHT };
}

static struct ui_rect size_rect(void)
{
    const struct ui_rect band = ribbon_rect();

    return (struct ui_rect){ group_left(PAINT_GROUP_SIZE), band.y + 4U,
        PAINT_TALL_WIDTH, 72U };
}

static struct ui_rect colour_button_rect(size_t item)
{
    const struct ui_rect band = ribbon_rect();

    return (struct ui_rect){ group_left(PAINT_GROUP_COLORS) +
        (uint32_t)item * PAINT_COLOUR_BUTTON, band.y + 6U,
        PAINT_COLOUR_BUTTON, PAINT_COLOUR_BUTTON };
}

static struct ui_rect edit_colours_rect(void)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_COLORS);

    return (struct ui_rect){ left + PAINT_COLOUR_BUTTON * 2U + 10U +
        PAINT_SWATCH * PAINT_SWATCH_COLUMNS + 10U, band.y + 4U, 54U, 72U };
}

static struct ui_rect zoom_track_rect(void)
{
    const struct ui_rect bar = status_rect();

    return (struct ui_rect){ bar.x + bar.width - PAINT_ZOOM_TRACK - 84U,
        bar.y, PAINT_ZOOM_TRACK, bar.height };
}

static struct ui_rect resize_dialog_rect(void)
{
    return (struct ui_rect){
        window_rect.x + (window_rect.width - PAINT_RESIZE_DIALOG_WIDTH) / 2U,
        window_rect.y + (window_rect.height - PAINT_RESIZE_DIALOG_HEIGHT) / 2U,
        PAINT_RESIZE_DIALOG_WIDTH, PAINT_RESIZE_DIALOG_HEIGHT
    };
}

static struct ui_rect resize_field_rect(size_t field)
{
    const struct ui_rect dialog = resize_dialog_rect();

    return (struct ui_rect){ dialog.x + 132U,
        dialog.y + 58U + (uint32_t)field * 38U, 132U, 28U };
}

static struct ui_rect resize_aspect_rect(void)
{
    const struct ui_rect dialog = resize_dialog_rect();

    return (struct ui_rect){ dialog.x + 22U, dialog.y + 139U, 190U, 24U };
}

static struct ui_rect resize_button_rect(size_t button)
{
    const struct ui_rect dialog = resize_dialog_rect();

    return (struct ui_rect){ dialog.x + 174U + (uint32_t)button * 78U,
        dialog.y + 174U, 70U, 28U };
}

/* ================================================================== PIECES */

static const char *tool_glyph(size_t tool)
{
    switch (tool) {
    case PAINT_TOOL_PENCIL:
        return "pencil";
    case PAINT_TOOL_FILL:
        return "paint-bucket";
    case PAINT_TOOL_TEXT:
        return "type";
    case PAINT_TOOL_ERASER:
        return "eraser";
    case PAINT_TOOL_PICKER:
        return "pipette";
    case PAINT_TOOL_MAGNIFIER:
    default:
        return "zoom-in";
    }
}

/*
 * Paint's shapes gallery, in Paint's own order.
 *
 * Paint has twenty-three shapes and shows twenty-one of them in three rows
 * of seven, the last two reachable by scrolling.  This gallery carries the
 * twenty-one it shows; heart and lightning are not here, and the scroll
 * strip beside the grid therefore has nothing to scroll - it is drawn
 * because Paint draws it, and it is the one control in this window that
 * does not do the thing it is drawn as.  Carrying all twenty-three and
 * scrolling a row is the fix; until then the strip is marked as inert
 * where it is drawn rather than left to look live.
 *
 * The order below is Paint's: line and curve first, then the closed
 * shapes, then the arrows, the stars, and the callouts.
 *
 * Lucide has no mark for three of them, so those three are drawn in Lucide's
 * own idiom - a 24-unit grid, stroke 2, round caps and joins - and go through
 * the same rasterizer.  Nothing here repeats: a gallery with the square in it
 * twice is a gallery that was padded rather than filled.
 */
static const char *const shape_glyph[PAINT_SHAPES] = {
    /* row one */
    "slash",               /* Line                        */
    "spline",              /* Curve                       */
    "circle",              /* Oval                        */
    "square",              /* Rectangle                   */
    "shape-rounded-rect",  /* Rounded rectangle           */
    "octagon",             /* Polygon                     */
    "triangle",            /* Triangle                    */
    /* row two */
    "shape-right-triangle",/* Right triangle              */
    "diamond",             /* Diamond                     */
    "pentagon",            /* Pentagon                    */
    "hexagon",             /* Hexagon                     */
    "arrow-right",         /* Right arrow                 */
    "arrow-left",          /* Left arrow                  */
    "arrow-up",            /* Up arrow                    */
    /* row three */
    "arrow-down",          /* Down arrow                  */
    "sparkle",             /* Four-point star             */
    "star",                /* Five-point star             */
    "shape-star-six",      /* Six-point star              */
    "message-square",      /* Rounded rectangular callout */
    "message-circle",      /* Oval callout                */
    "cloud"                /* Cloud callout               */
    /* Paint's Heart and Lightning would follow, on a fourth row. */
};

/*
 * A tall ribbon button: a mark over one or two lines of label, with an
 * optional dropdown chevron under it - which is how Paint says a button has
 * a menu behind it.
 */
static enum paint_status draw_tall(struct ui_rect box, struct ui_rect damage,
    const char *glyph, const char *first, const char *second, bool dropdown)
{
    enum paint_status status = draw_glyph(glyph, (struct ui_rect){ box.x,
        box.y + 4U, box.width, PAINT_TALL_ICON }, damage, ink_soft);
    uint32_t baseline = box.y + PAINT_TALL_ICON + 20U;

    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, centred_x(box.x, box.width, width_of(first)),
            baseline, first, ink);
    }
    if (status == PAINT_STATUS_OK && second != NULL) {
        baseline += 13U;
        status = text_at(damage,
            centred_x(box.x, box.width, width_of(second)), baseline, second,
            ink);
    }
    if (status == PAINT_STATUS_OK && dropdown) {
        status = draw_glyph("chevron-down", (struct ui_rect){ box.x,
            baseline + 2U, box.width, PAINT_MARK }, damage, ink_soft);
    }
    return status;
}

static enum paint_status draw_small(struct ui_rect box, struct ui_rect damage,
    const char *glyph, const char *label)
{
    const enum paint_status status = draw_glyph(glyph, (struct ui_rect){
        box.x, box.y, PAINT_SMALL_ICON, box.height }, damage, ink_soft);

    if (status != PAINT_STATUS_OK) {
        return status;
    }
    return text_at(damage, box.x + PAINT_SMALL_ICON + 5U,
        box.y + box.height / 2U + 5U, label, ink);
}

/* A group's name along the bottom, and the hairline that ends it. */
static enum paint_status draw_group_label(enum paint_group group,
    struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const char *label = group_label(group);
    const uint32_t left = group_left(group);
    const uint32_t width = group_width(group);
    const enum paint_status status = text_at(damage,
        centred_x(left, width, width_of(label)),
        band.y + band.height - 4U, label, ink_faint);

    if (status != PAINT_STATUS_OK || group + 1 == PAINT_GROUP_COUNT) {
        return status;
    }
    return fill((struct ui_rect){ left + width + PAINT_GROUP_GAP / 2U,
        band.y + 4U, 1U, band.height - PAINT_RIBBON_LABEL - 4U }, damage,
        rule_soft);
}

/* --- the seven groups --- */

static enum paint_status draw_clipboard(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_CLIPBOARD);
    enum paint_status status = draw_tall((struct ui_rect){ left, band.y + 4U,
        PAINT_TALL_WIDTH, 0U }, damage, "clipboard-paste", "Paste", NULL,
        true);

    if (status == PAINT_STATUS_OK) {
        status = draw_small((struct ui_rect){ left + PAINT_TALL_WIDTH + 4U,
            band.y + 14U, 60U, PAINT_SMALL_HEIGHT }, damage, "scissors",
            "Cut");
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_small((struct ui_rect){ left + PAINT_TALL_WIDTH + 4U,
            band.y + 14U + PAINT_SMALL_HEIGHT, 60U, PAINT_SMALL_HEIGHT },
            damage, "copy", "Copy");
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_CLIPBOARD, damage);
    }
    return status;
}

static enum paint_status draw_image_group(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_IMAGE);
    enum paint_status status = draw_tall((struct ui_rect){ left, band.y + 4U,
        PAINT_TALL_WIDTH, 0U }, damage, "square", "Select", NULL,
        true);

    if (status != PAINT_STATUS_OK) {
        return status;
    }
    if (select_mode) {
        status = outline(image_rect(0U), damage, accent);
    }
    /* Lucide has no dashed rectangle; Paint's Select is a plain marquee, so
     * the plain square stands in and the dropdown says the rest. */
    static const char *const glyphs[3] = { "crop", "scaling", "rotate-cw" };
    static const char *const labels[3] = { "Crop", "Resize", "Rotate" };

    for (size_t index = 0U; index < 3U && status == PAINT_STATUS_OK;
         ++index) {
        status = draw_small((struct ui_rect){ left + PAINT_TALL_WIDTH + 4U,
            band.y + 8U + (uint32_t)index * PAINT_SMALL_HEIGHT, 64U,
            PAINT_SMALL_HEIGHT }, damage, glyphs[index], labels[index]);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_IMAGE, damage);
    }
    return status;
}

static enum paint_status draw_tools(struct ui_rect damage)
{
    enum paint_status status = PAINT_STATUS_OK;

    for (size_t tool = 0U; tool < PAINT_TOOL_COUNT &&
            status == PAINT_STATUS_OK; ++tool) {
        const struct ui_rect cell = tool_rect(tool);
        const bool chosen = !select_mode && !shape_mode &&
            tool == (size_t)current_tool;

        /*
         * Phipia marks the held tool with the ACCENT.  Paint marks it with a
         * faint pressed plate that on a ribbon this busy takes a second look
         * to find, and the tool you are holding is the one thing about this
         * window you need to know without looking.
         */
        if (chosen) {
            status = fill(cell, damage, accent);
        } else if (tool == hover_tool) {
            status = fill(cell, damage, hover_fill);
        }
        if (status == PAINT_STATUS_OK) {
            status = draw_glyph(tool_glyph(tool), cell, damage,
                chosen ? on_accent : ink_soft);
        }
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_TOOLS, damage);
    }
    return status;
}

static enum paint_status draw_brushes(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    enum paint_status status = draw_tall((struct ui_rect){
        centred_x(group_left(PAINT_GROUP_BRUSHES),
            group_width(PAINT_GROUP_BRUSHES), PAINT_TALL_WIDTH),
        band.y + 4U, PAINT_TALL_WIDTH, 0U },
        damage, "brush", "Brushes", NULL, true);

    if (status != PAINT_STATUS_OK) {
        return status;
    }
    if (current_tool == PAINT_TOOL_PENCIL && stroke_radius > 1U &&
            !select_mode && !shape_mode) {
        status = outline(brushes_rect(), damage, accent);
        if (status != PAINT_STATUS_OK) {
            return status;
        }
    }
    return draw_group_label(PAINT_GROUP_BRUSHES, damage);
}

static enum paint_status draw_shapes(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t gallery_right = group_left(PAINT_GROUP_SHAPES) + 2U +
        PAINT_SHAPE_CELL * PAINT_SHAPE_COLUMNS;
    enum paint_status status = PAINT_STATUS_OK;

    for (size_t shape = 0U; shape < PAINT_SHAPES &&
            status == PAINT_STATUS_OK; ++shape) {
        const struct ui_rect cell = shape_rect(shape);

        if (shape_mode && shape == current_shape) {
            status = fill(cell, damage, accent);
        } else if (shape == hover_shape) {
            status = fill(cell, damage, hover_fill);
        }
        if (status == PAINT_STATUS_OK) {
            status = draw_glyph(shape_glyph[shape], cell, damage,
                shape_mode && shape == current_shape ? on_accent : ink_soft);
        }
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }
    /*
     * The gallery's scroll strip.  With every shape this gallery holds
     * already on screen there is nothing for it to scroll, so it is drawn
     * the way Paint draws a scroll arrow with nothing past it: in the
     * disabled ink, not the live one.  Two boxes rather than Paint's
     * three - the third is Paint's expander, and this gallery has no
     * flyout to expand into, so it is not drawn as though it had.
     */
    static const char *const arrows[2] = { "chevron-up", "chevron-down" };

    for (size_t index = 0U; index < 2U && status == PAINT_STATUS_OK;
         ++index) {
        const struct ui_rect box = { gallery_right,
            band.y + 8U + (uint32_t)index * 22U, PAINT_GALLERY_ARROWS, 22U };

        status = outline(box, damage, rule);
        if (status == PAINT_STATUS_OK) {
            status = draw_glyph(arrows[index], box, damage, ink_faint);
        }
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }

    const uint32_t side = gallery_right + PAINT_GALLERY_ARROWS + 6U;

    status = draw_small((struct ui_rect){ side, band.y + 12U, 70U,
        PAINT_SMALL_HEIGHT }, damage, "square", "Outline");
    if (status == PAINT_STATUS_OK && shape_outline) {
        status = outline(shape_option_rect(0U), damage, accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_small((struct ui_rect){ side,
            band.y + 12U + PAINT_SMALL_HEIGHT, 70U, PAINT_SMALL_HEIGHT },
            damage, "paint-bucket", "Fill");
    }
    if (status == PAINT_STATUS_OK && shape_fill) {
        status = outline(shape_option_rect(1U), damage, accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_SHAPES, damage);
    }
    return status;
}

/*
 * The Size button.
 *
 * Paint draws four rules of increasing weight inside it rather than an icon,
 * because the button IS the thing it chooses.  Stating that as geometry is
 * exact; there is no icon in any set that means "one, three, five and eight
 * pixels".
 */
static enum paint_status draw_size(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_SIZE);
    static const uint32_t weights[4] = { 1U, 3U, 5U, 8U };
    uint32_t pen = band.y + 10U;
    enum paint_status status = PAINT_STATUS_OK;

    for (size_t index = 0U; index < 4U && status == PAINT_STATUS_OK;
         ++index) {
        status = fill((struct ui_rect){ left + 12U, pen,
            PAINT_TALL_WIDTH - 24U, weights[index] }, damage, ink_soft);
        pen += weights[index] + 6U;
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage,
            centred_x(left, PAINT_TALL_WIDTH, width_of("Size")),
            band.y + 60U, "Size", ink);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("chevron-down", (struct ui_rect){ left,
            band.y + 63U, PAINT_TALL_WIDTH, PAINT_MARK }, damage,
            ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        status = outline(size_rect(), damage, accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_SIZE, damage);
    }
    return status;
}

static enum paint_status draw_colours(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_COLORS);
    static const char *const names[2] = { "Color 1", "Color 2" };
    const size_t chosen[2] = { colour_one, colour_two };
    enum paint_status status = PAINT_STATUS_OK;

    for (size_t index = 0U; index < 2U && status == PAINT_STATUS_OK;
         ++index) {
        const struct ui_rect box = {
            left + (uint32_t)index * PAINT_COLOUR_BUTTON, band.y + 6U,
            PAINT_COLOUR_BUTTON, PAINT_COLOUR_BUTTON };
        const struct ui_rect well = { box.x + 8U, box.y + 6U,
            box.width - 16U, 20U };
        char hex[8];

        status = fill(well, damage, swatches[chosen[index]]);
        if (status == PAINT_STATUS_OK) {
            status = outline(well, damage,
                index == colour_target ? accent : ink_faint);
        }
        if (status == PAINT_STATUS_OK) {
            status = text_at(damage,
                centred_x(box.x, box.width, width_of(names[index])),
                box.y + 40U, names[index], ink);
        }
        if (status != PAINT_STATUS_OK) {
            return status;
        }
        /*
         * And its hex under it, which Paint does not show anywhere without
         * opening Edit colors - and which is the one number anyone opening
         * Paint to match a colour actually wants.
         */
        hex_colour(hex, swatches[chosen[index]]);
        status = text_at(damage, centred_x(box.x, box.width, width_of(hex)),
            box.y + 54U, hex, ink_faint);
    }
    for (size_t index = 0U; index < PAINT_SWATCHES &&
            status == PAINT_STATUS_OK; ++index) {
        const struct ui_rect cell = swatch_rect(index);
        const struct ui_rect well = { cell.x + 1U, cell.y + 1U,
            cell.width - 2U, cell.height - 2U };

        status = fill(well, damage, swatches[index]);
        if (status == PAINT_STATUS_OK) {
            status = outline(well, damage,
                index == hover_swatch ? accent : ink_faint);
        }
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }

    const uint32_t edit = left + PAINT_COLOUR_BUTTON * 2U + 10U +
        PAINT_SWATCH * PAINT_SWATCH_COLUMNS + 10U;

    status = draw_tall((struct ui_rect){ edit, band.y + 4U, 54U, 0U },
        damage, "palette", "Edit", "colors", false);
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_COLORS, damage);
    }
    return status;
}

static enum paint_status draw_ribbon(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    enum paint_status status = fill(band, damage, ribbon_fill);

    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ band.x, band.y + band.height - 1U,
            band.width, 1U }, damage, rule);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_clipboard(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_image_group(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_tools(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_brushes(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_shapes(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_size(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_colours(damage);
    }
    return status;
}

/* ================================================================== CHROME */

static enum paint_status draw_caption(struct ui_rect damage)
{
    const struct ui_rect caption = caption_rect();
    /*
     * Paint's Quick Access Toolbar: its own mark, then save, undo, redo and
     * the customise chevron - inside the title bar, at its left end.
     *
     * Undo and redo are the ARROWS, not the circular rotate marks.  A pair
     * of circular arrows beside each other in a title bar reads as "rotate
     * left" and "rotate right", which in a drawing program is a thing Paint
     * can actually do to your picture - so the wrong icon here is not just
     * unclear, it means something else.  Windows draws these two in blue for
     * the same reason.
     */
    static const char *const quick[] = { "save", "undo-2", "redo-2",
        "chevron-down" };
    uint32_t pen = caption.x + 6U;
    enum paint_status status = fill(caption, damage, caption_fill);

    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("palette", (struct ui_rect){ pen, caption.y,
            PAINT_QAT_SLOT, caption.height }, damage, accent);
    }
    pen += PAINT_QAT_SLOT + 2U;
    for (size_t index = 0U; index < 4U && status == PAINT_STATUS_OK;
         ++index) {
        status = draw_glyph(quick[index], (struct ui_rect){ pen, caption.y,
            PAINT_QAT_SLOT, caption.height }, damage,
            index == 3U ? ink_faint :
                (index == 0U ? ink_soft : accent));
        pen += PAINT_QAT_SLOT;
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage,
            centred_x(caption.x, caption.width, width_of(window_title)),
            caption.y + caption.height / 2U + 5U, window_title,
            focused ? ink : ink_faint);
    }
    for (uint32_t index = 0U; index < 3U && status == PAINT_STATUS_OK;
         ++index) {
        const struct ui_rect button = caption_button_rect(index);
        const uint32_t size = 10U;
        const uint32_t bx = button.x + (button.width - size) / 2U;
        const uint32_t by = button.y + (button.height - size) / 2U;
        const struct paint_rgb mark = focused ? ink : ink_faint;

        if (index == 0U) {
            status = fill((struct ui_rect){ bx, by + size / 2U, size, 1U },
                damage, mark);
        } else if (index == 1U) {
            status = outline((struct ui_rect){ bx, by, size, size }, damage,
                mark);
        } else {
            for (uint32_t step = 0U; step < size &&
                    status == PAINT_STATUS_OK; ++step) {
                status = fill((struct ui_rect){ bx + step, by + step, 1U,
                    1U }, damage, mark);
                if (status == PAINT_STATUS_OK) {
                    status = fill((struct ui_rect){ bx + step,
                        by + size - 1U - step, 1U, 1U }, damage, mark);
                }
            }
        }
    }
    return status;
}

static enum paint_status draw_tabs(struct ui_rect damage)
{
    static const char *const tabs[] = { "File", "Home", "View" };
    const struct ui_rect strip = tabs_rect();
    uint32_t pen = strip.x + PAINT_PAD;
    enum paint_status status = fill(strip, damage, chrome);

    for (size_t index = 0U; index < 3U && status == PAINT_STATUS_OK;
         ++index) {
        const uint32_t width = width_of(tabs[index]) + PAINT_TAB_PAD * 2U;

        if (index == 0U) {
            /* File is a solid accent button rather than a tab, because it
             * opens a menu instead of switching the ribbon. */
            status = fill((struct ui_rect){ pen, strip.y, width,
                strip.height }, damage, accent);
            if (status == PAINT_STATUS_OK) {
                status = text_at(damage, pen + PAINT_TAB_PAD,
                    strip.y + strip.height / 2U + 5U, tabs[index], on_accent);
            }
        } else {
            if (index == 1U) {
                status = fill((struct ui_rect){ pen, strip.y, width,
                    strip.height }, damage, ribbon_fill);
            }
            if (status == PAINT_STATUS_OK) {
                status = text_at(damage, pen + PAINT_TAB_PAD,
                    strip.y + strip.height / 2U + 5U, tabs[index],
                    index == 1U ? ink : ink_soft);
            }
        }
        pen += width;
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }
    /* Paint puts a help mark and the ribbon's collapse chevron at the far
     * right of the tab strip, which nothing else in the shell does. */
    status = draw_glyph("help-circle", (struct ui_rect){
        strip.x + strip.width - 48U, strip.y, 20U, strip.height }, damage,
        ink_soft);
    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("chevron-up", (struct ui_rect){
            strip.x + strip.width - 24U, strip.y, 20U, strip.height },
            damage, ink_soft);
    }
    return status;
}

/*
 * The workspace: the grey Paint floats its sheet on, the sheet itself, and
 * the resize handles on its right and bottom edges only - Paint grows a
 * canvas right and down and never up or left, which is why there is no
 * handle on the other two sides.
 */
static enum paint_status draw_workspace(struct ui_rect damage)
{
    const struct ui_rect area = workspace_rect();
    const struct ui_rect page = paint_sheet_bounds();
    enum paint_status status = fill(area, damage, workspace);

    if (status == PAINT_STATUS_OK) {
        status = fill(page, damage, sheet);
    }
    const struct ui_rect clipped_page = intersect(page, damage);

    for (uint32_t y = 0U; y < clipped_page.height &&
            status == PAINT_STATUS_OK; ++y) {
        const uint32_t source_y = (uint32_t)(((uint64_t)
            (clipped_page.y + y - page.y) * 100U) / zoom_percent);

        if (source_y >= image_height) {
            break;
        }
        for (uint32_t x = 0U; x < clipped_page.width; ++x) {
            const uint32_t source_x = (uint32_t)(((uint64_t)
                (clipped_page.x + x - page.x) * 100U) / zoom_percent);

            if (source_x >= image_width) {
                break;
            }
            if (surface_pixel(canvas, clipped_page.x + x,
                    clipped_page.y + y,
                    canvas_pixels[(size_t)source_y * PAINT_CANVAS_WIDTH +
                        source_x]) != SURFACE_STATUS_OK) {
                status = PAINT_STATUS_SURFACE_FAILURE;
                break;
            }
        }
    }
    if (status == PAINT_STATUS_OK) {
        status = outline(page, damage, sheet_edge);
    }
    if (status == PAINT_STATUS_OK && selection_valid) {
        const struct ui_rect selected = {
            page.x + (uint32_t)((uint64_t)selection.x * zoom_percent / 100U),
            page.y + (uint32_t)((uint64_t)selection.y * zoom_percent / 100U),
            (uint32_t)((uint64_t)selection.width * zoom_percent / 100U),
            (uint32_t)((uint64_t)selection.height * zoom_percent / 100U)
        };

        if (selected.width != 0U && selected.height != 0U) {
            status = outline(selected, damage, accent);
        }
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }

    const struct ui_rect handles[3] = {
        { page.x + page.width - PAINT_HANDLE / 2U,
          page.y + page.height / 2U - PAINT_HANDLE / 2U,
          PAINT_HANDLE, PAINT_HANDLE },
        { page.x + page.width / 2U - PAINT_HANDLE / 2U,
          page.y + page.height - PAINT_HANDLE / 2U,
          PAINT_HANDLE, PAINT_HANDLE },
        { page.x + page.width - PAINT_HANDLE / 2U,
          page.y + page.height - PAINT_HANDLE / 2U,
          PAINT_HANDLE, PAINT_HANDLE }
    };

    for (size_t index = 0U; index < 3U && status == PAINT_STATUS_OK;
         ++index) {
        status = fill(handles[index], damage, sheet);
        if (status == PAINT_STATUS_OK) {
            status = outline(handles[index], damage, sheet_edge);
        }
    }
    return status;
}

/* Print an unsigned number right where it is asked for; the kernel has no
 * formatter and the status bar needs three of them. */
static void number_text(char *out, uint32_t value, const char *suffix)
{
    char digits[12];
    size_t count = 0U;
    size_t at = 0U;

    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count > 0U) {
        out[at++] = digits[--count];
    }
    while (*suffix != '\0') {
        out[at++] = *suffix++;
    }
    out[at] = '\0';
}

static enum paint_status draw_resize_dialog(struct ui_rect damage)
{
    const struct ui_rect dialog = resize_dialog_rect();
    const struct ui_rect aspect = resize_aspect_rect();
    char value[16];
    enum paint_status status = fill((struct ui_rect){ dialog.x + 4U,
        dialog.y + 4U, dialog.width, dialog.height }, damage, ink_faint);

    if (status == PAINT_STATUS_OK) {
        status = fill(dialog, damage, caption_fill);
    }
    if (status == PAINT_STATUS_OK) {
        status = outline(dialog, damage, sheet_edge);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ dialog.x + 1U, dialog.y + 1U,
            dialog.width - 2U, 36U }, damage, accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, dialog.x + 14U, dialog.y + 24U,
            "Resize image", on_accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, dialog.x + 22U, dialog.y + 78U,
            "Width", ink);
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, dialog.x + 22U, dialog.y + 116U,
            "Height", ink);
    }
    for (size_t field = 0U; field < 2U && status == PAINT_STATUS_OK;
         ++field) {
        const struct ui_rect box = resize_field_rect(field);

        status = fill(box, damage, sheet);
        if (status == PAINT_STATUS_OK) {
            status = outline(box, damage,
                resize_field == field ? accent : rule);
        }
        if (status == PAINT_STATUS_OK) {
            number_text(value, field == 0U ? resize_width_value :
                resize_height_value, "");
            status = text_at(damage, box.x + 8U, box.y + 19U, value, ink);
        }
        if (status == PAINT_STATUS_OK) {
            status = text_at(damage, box.x + box.width + 8U, box.y + 19U,
                "px", ink_soft);
        }
    }
    if (status == PAINT_STATUS_OK) {
        status = outline((struct ui_rect){ aspect.x, aspect.y + 4U, 16U, 16U },
            damage, resize_keep_aspect ? accent : rule);
    }
    if (status == PAINT_STATUS_OK && resize_keep_aspect) {
        status = text_at(damage, aspect.x + 4U, aspect.y + 17U, "x", accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, aspect.x + 24U, aspect.y + 18U,
            "Maintain aspect ratio", ink);
    }
    for (size_t button = 0U; button < 2U && status == PAINT_STATUS_OK;
         ++button) {
        const struct ui_rect box = resize_button_rect(button);

        status = fill(box, damage, button == 0U ? accent : chrome);
        if (status == PAINT_STATUS_OK) {
            status = outline(box, damage, button == 0U ? accent : rule);
        }
        if (status == PAINT_STATUS_OK) {
            const char *const label = button == 0U ? "Apply" : "Cancel";

            status = text_at(damage, centred_x(box.x, box.width,
                width_of(label)), box.y + 19U, label,
                button == 0U ? on_accent : ink);
        }
    }
    return status;
}

static enum paint_status draw_status(struct ui_rect damage)
{
    const struct ui_rect bar = status_rect();
    const uint32_t baseline = bar.y + bar.height / 2U + 5U;
    char text[24];
    enum paint_status status = fill(bar, damage, chrome);

    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ bar.x, bar.y, bar.width, 1U }, damage,
            rule);
    }
    /* The pointer readout, then the canvas size, then the file size - the
     * three things Paint's status bar carries, in its order. */
    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("move", (struct ui_rect){ bar.x + 6U, bar.y,
            PAINT_MARK,
            bar.height }, damage, ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, bar.x + 24U, baseline, "482, 216", ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("scaling", (struct ui_rect){ bar.x + 96U, bar.y,
            PAINT_MARK, bar.height }, damage, ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        number_text(text, image_width, " x ");
        status = text_at(damage, bar.x + 114U, baseline, text, ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        number_text(text, image_height, "px");
        status = text_at(damage, bar.x + 114U + width_of("0000 x "), baseline,
            text, ink_soft);
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }

    /*
     * The zoom, at the right end: minus, a slider, plus, then the
     * percentage.  Phipia fills the used half of the track with the accent -
     * Paint draws a grey thumb on a grey track and the control reads as
     * decoration until you touch it.
     */
    const uint32_t track_x = bar.x + bar.width - PAINT_ZOOM_TRACK - 84U;
    const uint32_t middle = bar.y + bar.height / 2U;
    /*
     * Paint's zoom slider is not linear.  It has seven notches - 12, 25, 50,
     * 100, 200, 400, 800 - each a doubling of the last, so 100% sits in the
     * MIDDLE of the track rather than an eighth of the way along it.  A
     * linear slider puts the default hard against the left stop, which is
     * the giveaway that a copy measured the numbers and not the control.
     */
    static const uint32_t notches[7] = { 12U, 25U, 50U, 100U, 200U, 400U,
        800U };
    size_t notch = 0U;

    for (size_t index = 0U; index < 7U; ++index) {
        if (zoom_percent >= notches[index]) {
            notch = index;
        }
    }

    const uint32_t lit = PAINT_ZOOM_TRACK * (uint32_t)notch / 6U;

    status = text_at(damage, track_x - 16U, baseline, "-", ink_soft);
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ track_x, middle - 1U,
            PAINT_ZOOM_TRACK, 2U }, damage, rule);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ track_x, middle - 1U, lit, 2U },
            damage, accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){
            track_x + (lit > PAINT_ZOOM_THUMB / 2U ?
                lit - PAINT_ZOOM_THUMB / 2U : 0U),
            middle - 7U, PAINT_ZOOM_THUMB, 14U }, damage, accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, track_x + PAINT_ZOOM_TRACK + 8U, baseline,
            "+", ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        number_text(text, zoom_percent, "%");
        status = text_at(damage, bar.x + bar.width - 8U - width_of(text),
            baseline, text, ink_soft);
    }
    return status;
}

enum paint_status paint_draw(struct ui_rect damage)
{
    enum paint_status status;

    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    status = fill(window_rect, damage,
        focused ? border_active : border_inactive);
    if (status == PAINT_STATUS_OK) {
        status = draw_caption(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_tabs(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_ribbon(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_workspace(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_status(damage);
    }
    if (status == PAINT_STATUS_OK && resize_dialog_open) {
        status = draw_resize_dialog(damage);
    }
    return status;
}

/* ================================================================== INPUT */

static bool canvas_point(struct ui_point point, uint32_t *x, uint32_t *y)
{
    const struct ui_rect page = paint_sheet_bounds();

    if (!holds(page, point)) {
        return false;
    }
    *x = (uint32_t)(((uint64_t)((uint32_t)point.x - page.x) * 100U) /
        zoom_percent);
    *y = (uint32_t)(((uint64_t)((uint32_t)point.y - page.y) * 100U) /
        zoom_percent);
    return *x < image_width && *y < image_height;
}

static void paint_dot_canvas(int32_t center_x, int32_t center_y,
    uint32_t colour, uint32_t radius)
{
    if (center_x < 0 || center_y < 0 || (uint32_t)center_x >= image_width ||
            (uint32_t)center_y >= image_height) {
        return;
    }
    const uint32_t cx = (uint32_t)center_x;
    const uint32_t cy = (uint32_t)center_y;
    const uint32_t left = cx > radius ? cx - radius : 0U;
    const uint32_t top = cy > radius ? cy - radius : 0U;
    uint32_t right = cx + radius + 1U;
    uint32_t bottom = cy + radius + 1U;

    if (right > image_width) {
        right = image_width;
    }
    if (bottom > image_height) {
        bottom = image_height;
    }
    for (uint32_t y = top; y < bottom; ++y) {
        for (uint32_t x = left; x < right; ++x) {
            canvas_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x] = colour;
        }
    }
}

static void paint_dot(struct ui_point point, uint32_t colour, uint32_t radius)
{
    uint32_t center_x;
    uint32_t center_y;

    if (canvas_point(point, &center_x, &center_y)) {
        paint_dot_canvas((int32_t)center_x, (int32_t)center_y, colour,
            radius);
    }
}

static void paint_line_canvas(struct ui_point from, struct ui_point to,
    uint32_t colour, uint32_t radius)
{
    int32_t x = from.x;
    int32_t y = from.y;
    const int32_t dx = to.x > from.x ? to.x - from.x : from.x - to.x;
    const int32_t dy = to.y > from.y ? from.y - to.y : to.y - from.y;
    const int32_t step_x = from.x < to.x ? 1 : -1;
    const int32_t step_y = from.y < to.y ? 1 : -1;
    int32_t error = dx + dy;

    for (;;) {
        paint_dot_canvas(x, y, colour, radius);
        if (x == to.x && y == to.y) {
            break;
        }
        const int32_t twice = error * 2;

        if (twice >= dy) {
            error += dy;
            x += step_x;
        }
        if (twice <= dx) {
            error += dx;
            y += step_y;
        }
    }
}

static void paint_line(struct ui_point from, struct ui_point to,
    uint32_t colour, uint32_t radius)
{
    int32_t x = from.x;
    int32_t y = from.y;
    const int32_t dx = to.x > from.x ? to.x - from.x : from.x - to.x;
    const int32_t dy = to.y > from.y ? from.y - to.y : to.y - from.y;
    const int32_t step_x = from.x < to.x ? 1 : -1;
    const int32_t step_y = from.y < to.y ? 1 : -1;
    int32_t error = dx + dy;

    for (;;) {
        paint_dot((struct ui_point){ x, y }, colour, radius);
        if (x == to.x && y == to.y) {
            break;
        }
        const int32_t twice = error * 2;

        if (twice >= dy) {
            error += dy;
            x += step_x;
        }
        if (twice <= dx) {
            error += dx;
            y += step_y;
        }
    }
}

static struct ui_rect canvas_rect(struct ui_point first,
    struct ui_point second)
{
    const uint32_t left = first.x < second.x ? (uint32_t)first.x :
        (uint32_t)second.x;
    const uint32_t top = first.y < second.y ? (uint32_t)first.y :
        (uint32_t)second.y;
    const uint32_t right = first.x > second.x ? (uint32_t)first.x :
        (uint32_t)second.x;
    const uint32_t bottom = first.y > second.y ? (uint32_t)first.y :
        (uint32_t)second.y;

    return (struct ui_rect){ left, top, right - left + 1U,
        bottom - top + 1U };
}

static void clear_canvas_pixels(void)
{
    const uint32_t white = pack_rgb(sheet);

    for (size_t index = 0U;
         index < (size_t)PAINT_CANVAS_WIDTH * PAINT_CANVAS_HEIGHT; ++index) {
        canvas_pixels[index] = white;
    }
}

static struct ui_rect copy_area(void)
{
    if (selection_valid) {
        return selection;
    }
    return (struct ui_rect){ 0U, 0U,
        image_width < PAINT_CLIPBOARD_WIDTH ? image_width :
            PAINT_CLIPBOARD_WIDTH,
        image_height < PAINT_CLIPBOARD_HEIGHT ? image_height :
            PAINT_CLIPBOARD_HEIGHT };
}

static void copy_selection(bool cut)
{
    const struct ui_rect source = copy_area();

    clipboard_width = source.width < PAINT_CLIPBOARD_WIDTH ? source.width :
        PAINT_CLIPBOARD_WIDTH;
    clipboard_height = source.height < PAINT_CLIPBOARD_HEIGHT ?
        source.height : PAINT_CLIPBOARD_HEIGHT;
    if (clipboard_width == 0U || clipboard_height == 0U) {
        clipboard_valid = false;
        return;
    }
    if (cut) {
        capture_undo();
    }
    for (uint32_t y = 0U; y < clipboard_height; ++y) {
        for (uint32_t x = 0U; x < clipboard_width; ++x) {
            clipboard_pixels[(size_t)y * PAINT_CLIPBOARD_WIDTH + x] =
                canvas_pixels[(size_t)(source.y + y) * PAINT_CANVAS_WIDTH +
                    source.x + x];
            if (cut) {
                canvas_pixels[(size_t)(source.y + y) * PAINT_CANVAS_WIDTH +
                    source.x + x] = pack_rgb(sheet);
            }
        }
    }
    clipboard_valid = true;
    if (cut) {
        image_dirty = true;
    }
}

static void paste_selection(void)
{
    if (!clipboard_valid) {
        return;
    }
    const uint32_t left = selection_valid ? selection.x : 0U;
    const uint32_t top = selection_valid ? selection.y : 0U;

    capture_undo();
    for (uint32_t y = 0U; y < clipboard_height && top + y < image_height;
         ++y) {
        for (uint32_t x = 0U; x < clipboard_width && left + x < image_width;
             ++x) {
            canvas_pixels[(size_t)(top + y) * PAINT_CANVAS_WIDTH + left + x] =
                clipboard_pixels[(size_t)y * PAINT_CLIPBOARD_WIDTH + x];
        }
    }
    selection = (struct ui_rect){ left, top,
        clipboard_width < image_width - left ? clipboard_width :
            image_width - left,
        clipboard_height < image_height - top ? clipboard_height :
            image_height - top };
    selection_valid = selection.width != 0U && selection.height != 0U;
    image_dirty = true;
}

static void crop_selection(void)
{
    if (!selection_valid || selection.width == 0U ||
            selection.height == 0U) {
        return;
    }
    const uint32_t width = selection.width < PAINT_CANVAS_WIDTH ?
        selection.width : PAINT_CANVAS_WIDTH;
    const uint32_t height = selection.height < PAINT_CANVAS_HEIGHT ?
        selection.height : PAINT_CANVAS_HEIGHT;

    capture_undo();
    for (uint32_t y = 0U; y < height; ++y) {
        for (uint32_t x = 0U; x < width; ++x) {
            redo_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x] =
                canvas_pixels[(size_t)(selection.y + y) *
                    PAINT_CANVAS_WIDTH + selection.x + x];
        }
    }
    clear_canvas_pixels();
    for (uint32_t y = 0U; y < height; ++y) {
        for (uint32_t x = 0U; x < width; ++x) {
            canvas_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x] =
                redo_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x];
        }
    }
    image_width = width;
    image_height = height;
    selection_valid = false;
    redo_valid = false;
    image_dirty = true;
}

enum paint_status paint_resize_image(uint32_t width, uint32_t height)
{
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    if (width == 0U || height == 0U || width > PAINT_CANVAS_WIDTH ||
            height > PAINT_CANVAS_HEIGHT) {
        return PAINT_STATUS_BAD_INDEX;
    }
    if (width == image_width && height == image_height) {
        return PAINT_STATUS_OK;
    }

    capture_undo();
    for (uint32_t y = 0U; y < height; ++y) {
        const uint32_t source_y = (uint32_t)((uint64_t)y * image_height /
            height);

        for (uint32_t x = 0U; x < width; ++x) {
            const uint32_t source_x = (uint32_t)((uint64_t)x * image_width /
                width);

            redo_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x] =
                canvas_pixels[(size_t)source_y * PAINT_CANVAS_WIDTH +
                    source_x];
        }
    }
    clear_canvas_pixels();
    for (uint32_t y = 0U; y < height; ++y) {
        for (uint32_t x = 0U; x < width; ++x) {
            canvas_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x] =
                redo_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x];
        }
    }
    image_width = width;
    image_height = height;
    selection_valid = false;
    redo_valid = false;
    image_dirty = true;
    return PAINT_STATUS_OK;
}

static bool resize_sync_other(void)
{
    uint32_t value;

    if (!resize_keep_aspect) {
        return true;
    }
    if (resize_field == 0U) {
        if (resize_width_value == 0U) {
            resize_height_value = 0U;
            return true;
        }
        value = (uint32_t)(((uint64_t)image_height * resize_width_value +
            image_width / 2U) / image_width);
        if (value == 0U || value > PAINT_CANVAS_HEIGHT) {
            return false;
        }
        resize_height_value = value;
    } else {
        if (resize_height_value == 0U) {
            resize_width_value = 0U;
            return true;
        }
        value = (uint32_t)(((uint64_t)image_width * resize_height_value +
            image_height / 2U) / image_height);
        if (value == 0U || value > PAINT_CANVAS_WIDTH) {
            return false;
        }
        resize_width_value = value;
    }
    return true;
}

static enum paint_status apply_resize_dialog(void)
{
    const enum paint_status status = paint_resize_image(resize_width_value,
        resize_height_value);

    if (status == PAINT_STATUS_OK) {
        resize_dialog_open = false;
    }
    return status;
}

static void rotate_image(void)
{
    uint32_t width = image_height;
    uint32_t height = image_width;

    if (width > PAINT_CANVAS_WIDTH) {
        width = PAINT_CANVAS_WIDTH;
    }
    if (height > PAINT_CANVAS_HEIGHT) {
        height = PAINT_CANVAS_HEIGHT;
    }
    capture_undo();
    for (uint32_t y = 0U; y < height; ++y) {
        const uint32_t source_x = (uint32_t)((uint64_t)y * image_width /
            height);

        for (uint32_t x = 0U; x < width; ++x) {
            const uint32_t source_y = image_height - 1U - (uint32_t)(
                (uint64_t)x * image_height / width);

            redo_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x] =
                canvas_pixels[(size_t)source_y * PAINT_CANVAS_WIDTH +
                    source_x];
        }
    }
    clear_canvas_pixels();
    for (uint32_t y = 0U; y < height; ++y) {
        for (uint32_t x = 0U; x < width; ++x) {
            canvas_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x] =
                redo_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x];
        }
    }
    image_width = width;
    image_height = height;
    selection_valid = false;
    redo_valid = false;
    image_dirty = true;
}

static void flood_fill(uint32_t start_x, uint32_t start_y, uint32_t colour)
{
    const uint32_t target = canvas_pixels[
        (size_t)start_y * PAINT_CANVAS_WIDTH + start_x];
    size_t head = 0U;
    size_t tail = 0U;

    if (target == colour) {
        return;
    }
    redo_pixels[tail++] = start_y * PAINT_CANVAS_WIDTH + start_x;
    canvas_pixels[(size_t)start_y * PAINT_CANVAS_WIDTH + start_x] = colour;
    while (head < tail) {
        const uint32_t cell = redo_pixels[head++];
        const uint32_t x = cell % PAINT_CANVAS_WIDTH;
        const uint32_t y = cell / PAINT_CANVAS_WIDTH;
        const uint32_t neighbours[4] = {
            x == 0U ? cell : cell - 1U,
            x + 1U >= image_width ? cell : cell + 1U,
            y == 0U ? cell : cell - PAINT_CANVAS_WIDTH,
            y + 1U >= image_height ? cell : cell + PAINT_CANVAS_WIDTH
        };

        for (size_t index = 0U; index < 4U; ++index) {
            const uint32_t neighbour = neighbours[index];

            if (neighbour == cell || canvas_pixels[neighbour] != target) {
                continue;
            }
            canvas_pixels[neighbour] = colour;
            redo_pixels[tail++] = neighbour;
        }
    }
    redo_valid = false;
}

static void paint_rectangle_shape(struct ui_rect area)
{
    const uint32_t outline_colour = pack_rgb(swatches[colour_one]);
    const uint32_t fill_colour = pack_rgb(swatches[colour_two]);
    const uint32_t right = area.x + area.width - 1U;
    const uint32_t bottom = area.y + area.height - 1U;

    if (shape_fill) {
        for (uint32_t y = area.y; y <= bottom; ++y) {
            for (uint32_t x = area.x; x <= right; ++x) {
                canvas_pixels[(size_t)y * PAINT_CANVAS_WIDTH + x] =
                    fill_colour;
            }
        }
    }
    if (shape_outline || !shape_fill) {
        paint_line_canvas((struct ui_point){ (int32_t)area.x,
            (int32_t)area.y }, (struct ui_point){ (int32_t)right,
            (int32_t)area.y }, outline_colour, stroke_radius);
        paint_line_canvas((struct ui_point){ (int32_t)right,
            (int32_t)area.y }, (struct ui_point){ (int32_t)right,
            (int32_t)bottom }, outline_colour, stroke_radius);
        paint_line_canvas((struct ui_point){ (int32_t)right,
            (int32_t)bottom }, (struct ui_point){ (int32_t)area.x,
            (int32_t)bottom }, outline_colour, stroke_radius);
        paint_line_canvas((struct ui_point){ (int32_t)area.x,
            (int32_t)bottom }, (struct ui_point){ (int32_t)area.x,
            (int32_t)area.y }, outline_colour, stroke_radius);
    }
}

static void paint_ellipse_shape(struct ui_rect area)
{
    const uint32_t outline_colour = pack_rgb(swatches[colour_one]);
    const uint32_t fill_colour = pack_rgb(swatches[colour_two]);
    const int32_t center_x = (int32_t)(area.x + area.width / 2U);
    const int32_t center_y = (int32_t)(area.y + area.height / 2U);
    const int32_t radius_x = area.width > 2U ? (int32_t)area.width / 2 : 1;
    const int32_t radius_y = area.height > 2U ? (int32_t)area.height / 2 : 1;
    const uint64_t rx_squared = (uint64_t)(radius_x * radius_x);
    const uint64_t ry_squared = (uint64_t)(radius_y * radius_y);
    const uint64_t edge = rx_squared * ry_squared;
    const uint64_t band = (rx_squared + ry_squared) *
        (stroke_radius + 1U) * 3U;

    for (int32_t y = center_y - radius_y; y <= center_y + radius_y; ++y) {
        if (y < 0 || (uint32_t)y >= image_height) {
            continue;
        }
        for (int32_t x = center_x - radius_x; x <= center_x + radius_x;
             ++x) {
            if (x < 0 || (uint32_t)x >= image_width) {
                continue;
            }
            const int32_t dx = x - center_x;
            const int32_t dy = y - center_y;
            const uint64_t value = (uint64_t)(dx * dx) * ry_squared +
                (uint64_t)(dy * dy) * rx_squared;

            if (value > edge) {
                continue;
            }
            if (shape_fill) {
                canvas_pixels[(size_t)y * PAINT_CANVAS_WIDTH +
                    (uint32_t)x] = fill_colour;
            }
            if (shape_outline || !shape_fill) {
                if (value + band >= edge) {
                    canvas_pixels[(size_t)y * PAINT_CANVAS_WIDTH +
                        (uint32_t)x] = outline_colour;
                }
            }
        }
    }
}

static void paint_shape_between(struct ui_point first, struct ui_point last)
{
    const struct ui_rect area = canvas_rect(first, last);
    const uint32_t colour = pack_rgb(swatches[colour_one]);
    const int32_t left = (int32_t)area.x;
    const int32_t top = (int32_t)area.y;
    const int32_t right = (int32_t)(area.x + area.width - 1U);
    const int32_t bottom = (int32_t)(area.y + area.height - 1U);
    const int32_t middle_x = left + (right - left) / 2;
    const int32_t middle_y = top + (bottom - top) / 2;
    const struct ui_point a = { left, top };
    const struct ui_point b = { right, bottom };

    if (current_shape == 0U) {
        paint_line_canvas(first, last, colour, stroke_radius);
    } else if (current_shape == 1U) {
        paint_line_canvas(first, (struct ui_point){ middle_x, top }, colour,
            stroke_radius);
        paint_line_canvas((struct ui_point){ middle_x, top }, last, colour,
            stroke_radius);
    } else if (current_shape == 2U || current_shape == 19U ||
            current_shape == 20U) {
        paint_ellipse_shape(area);
        if (current_shape == 19U) {
            paint_line_canvas((struct ui_point){ middle_x, bottom - 2 },
                (struct ui_point){ middle_x - 8, bottom + 8 }, colour,
                stroke_radius);
        } else if (current_shape == 20U) {
            const struct ui_rect puff = { area.x + area.width / 4U,
                area.y, area.width / 2U + 1U, area.height / 2U + 1U };

            paint_ellipse_shape(puff);
        }
    } else if (current_shape == 3U || current_shape == 4U ||
            current_shape == 18U) {
        paint_rectangle_shape(area);
        if (current_shape == 18U) {
            paint_line_canvas((struct ui_point){ middle_x, bottom },
                (struct ui_point){ middle_x - 8, bottom + 8 }, colour,
                stroke_radius);
        }
    } else if (current_shape == 6U) {
        paint_line_canvas((struct ui_point){ middle_x, top },
            (struct ui_point){ right, bottom }, colour, stroke_radius);
        paint_line_canvas((struct ui_point){ right, bottom },
            (struct ui_point){ left, bottom }, colour, stroke_radius);
        paint_line_canvas((struct ui_point){ left, bottom },
            (struct ui_point){ middle_x, top }, colour, stroke_radius);
    } else if (current_shape == 7U) {
        paint_line_canvas(a, (struct ui_point){ left, bottom }, colour,
            stroke_radius);
        paint_line_canvas((struct ui_point){ left, bottom }, b, colour,
            stroke_radius);
        paint_line_canvas(b, a, colour, stroke_radius);
    } else if (current_shape == 8U || current_shape == 9U ||
            current_shape == 10U) {
        paint_line_canvas((struct ui_point){ middle_x, top },
            (struct ui_point){ right, middle_y }, colour, stroke_radius);
        paint_line_canvas((struct ui_point){ right, middle_y },
            (struct ui_point){ middle_x, bottom }, colour, stroke_radius);
        paint_line_canvas((struct ui_point){ middle_x, bottom },
            (struct ui_point){ left, middle_y }, colour, stroke_radius);
        paint_line_canvas((struct ui_point){ left, middle_y },
            (struct ui_point){ middle_x, top }, colour, stroke_radius);
    } else if (current_shape >= 11U && current_shape <= 14U) {
        paint_line_canvas(first, last, colour, stroke_radius);
        if (current_shape == 11U || current_shape == 12U) {
            const int32_t tip = current_shape == 11U ? right : left;
            const int32_t wing = current_shape == 11U ? right - 12 :
                left + 12;

            paint_line_canvas((struct ui_point){ tip, middle_y },
                (struct ui_point){ wing, middle_y - 10 }, colour,
                stroke_radius);
            paint_line_canvas((struct ui_point){ tip, middle_y },
                (struct ui_point){ wing, middle_y + 10 }, colour,
                stroke_radius);
        } else {
            const int32_t tip = current_shape == 13U ? top : bottom;
            const int32_t wing = current_shape == 13U ? top + 12 :
                bottom - 12;

            paint_line_canvas((struct ui_point){ middle_x, tip },
                (struct ui_point){ middle_x - 10, wing }, colour,
                stroke_radius);
            paint_line_canvas((struct ui_point){ middle_x, tip },
                (struct ui_point){ middle_x + 10, wing }, colour,
                stroke_radius);
        }
    } else if (current_shape >= 15U && current_shape <= 17U) {
        paint_line_canvas((struct ui_point){ middle_x, top },
            (struct ui_point){ middle_x, bottom }, colour, stroke_radius);
        paint_line_canvas((struct ui_point){ left, middle_y },
            (struct ui_point){ right, middle_y }, colour, stroke_radius);
        paint_line_canvas(a, b, colour, stroke_radius);
        paint_line_canvas((struct ui_point){ right, top },
            (struct ui_point){ left, bottom }, colour, stroke_radius);
    } else {
        paint_rectangle_shape(area);
    }
}

enum paint_status paint_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const size_t was_tool = hover_tool;
    const size_t was_shape = hover_shape;
    const size_t was_swatch = hover_swatch;

    if (damage == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (selecting) {
        uint32_t x;
        uint32_t y;

        if (canvas_point(point, &x, &y)) {
            selection = canvas_rect(selection_origin,
                (struct ui_point){ (int32_t)x, (int32_t)y });
            selection_valid = true;
            *damage = paint_sheet_bounds();
        }
    } else if (painting) {
        const uint32_t colour = current_tool == PAINT_TOOL_ERASER ?
            pack_rgb(sheet) : pack_rgb(swatches[colour_one]);
        const uint32_t radius = current_tool == PAINT_TOOL_ERASER ?
            stroke_radius + 3U : stroke_radius;

        paint_line(last_paint_point, point, colour, radius);
        last_paint_point = point;
        *damage = paint_sheet_bounds();
        image_dirty = true;
    }
    hover_tool = (size_t)-1;
    hover_shape = (size_t)-1;
    hover_swatch = (size_t)-1;
    for (size_t tool = 0U; tool < PAINT_TOOL_COUNT; ++tool) {
        if (holds(tool_rect(tool), point)) {
            hover_tool = tool;
            break;
        }
    }
    for (size_t shape = 0U; shape < PAINT_SHAPES; ++shape) {
        if (holds(shape_rect(shape), point)) {
            hover_shape = shape;
            break;
        }
    }
    for (size_t index = 0U; index < PAINT_SWATCHES; ++index) {
        if (holds(swatch_rect(index), point)) {
            hover_swatch = index;
            break;
        }
    }
    if (was_tool != hover_tool || was_shape != hover_shape ||
            was_swatch != hover_swatch) {
        *damage = painting ? window_rect : ribbon_rect();
    }
    return PAINT_STATUS_OK;
}

enum paint_status paint_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (resize_dialog_open) {
        for (size_t field = 0U; field < 2U; ++field) {
            if (holds(resize_field_rect(field), point)) {
                resize_field = field;
                *damage = resize_dialog_rect();
                return PAINT_STATUS_OK;
            }
        }
        if (holds(resize_aspect_rect(), point)) {
            const uint32_t old_width = resize_width_value;
            const uint32_t old_height = resize_height_value;

            resize_keep_aspect = !resize_keep_aspect;
            if (resize_keep_aspect && !resize_sync_other()) {
                resize_width_value = old_width;
                resize_height_value = old_height;
                resize_keep_aspect = false;
            }
            *damage = resize_dialog_rect();
            return PAINT_STATUS_OK;
        }
        if (holds(resize_button_rect(0U), point)) {
            const enum paint_status status = apply_resize_dialog();

            *damage = status == PAINT_STATUS_OK ? window_rect :
                resize_dialog_rect();
            return status;
        }
        if (holds(resize_button_rect(1U), point)) {
            resize_dialog_open = false;
            *damage = window_rect;
            return PAINT_STATUS_OK;
        }
        *damage = resize_dialog_rect();
        return PAINT_STATUS_OK;
    }
    for (size_t index = 0U; index < 3U; ++index) {
        if (!holds(quick_rect(index), point)) {
            continue;
        }
        if (index == 0U) {
            save_requested = true;
        } else if (index == 1U) {
            undo_image();
        } else {
            redo_image();
        }
        *damage = window_rect;
        return PAINT_STATUS_OK;
    }
    for (size_t item = 0U; item < 3U; ++item) {
        if (!holds(clipboard_rect(item), point)) {
            continue;
        }
        if (item == 0U) {
            paste_selection();
        } else {
            copy_selection(item == 1U);
        }
        *damage = window_rect;
        return PAINT_STATUS_OK;
    }
    for (size_t item = 0U; item < 4U; ++item) {
        if (!holds(image_rect(item), point)) {
            continue;
        }
        if (item == 0U) {
            select_mode = true;
            shape_mode = false;
            text_active = false;
        } else if (item == 1U) {
            crop_selection();
        } else if (item == 2U) {
            resize_dialog_open = true;
            resize_keep_aspect = true;
            resize_field = 0U;
            resize_width_value = image_width;
            resize_height_value = image_height;
        } else {
            rotate_image();
        }
        *damage = window_rect;
        return PAINT_STATUS_OK;
    }
    if (holds(brushes_rect(), point)) {
        current_tool = PAINT_TOOL_PENCIL;
        select_mode = false;
        shape_mode = false;
        text_active = false;
        stroke_radius = stroke_radius >= 8U ? 1U : stroke_radius * 2U;
        *damage = ribbon_rect();
        return PAINT_STATUS_OK;
    }
    for (size_t item = 0U; item < 2U; ++item) {
        if (!holds(shape_option_rect(item), point)) {
            continue;
        }
        if (item == 0U) {
            shape_outline = !shape_outline;
        } else {
            shape_fill = !shape_fill;
        }
        *damage = ribbon_rect();
        return PAINT_STATUS_OK;
    }
    if (holds(size_rect(), point)) {
        stroke_radius = stroke_radius == 1U ? 2U :
            (stroke_radius == 2U ? 4U :
                (stroke_radius == 4U ? 8U : 1U));
        *damage = ribbon_rect();
        return PAINT_STATUS_OK;
    }
    for (size_t item = 0U; item < 2U; ++item) {
        if (holds(colour_button_rect(item), point)) {
            colour_target = item;
            *damage = ribbon_rect();
            return PAINT_STATUS_OK;
        }
    }
    if (holds(edit_colours_rect(), point)) {
        if (colour_target == 0U) {
            colour_one = (colour_one + 1U) % PAINT_SWATCHES;
        } else {
            colour_two = (colour_two + 1U) % PAINT_SWATCHES;
        }
        *damage = ribbon_rect();
        return PAINT_STATUS_OK;
    }
    {
        const struct ui_rect track = zoom_track_rect();
        const struct ui_rect minus = { track.x - 20U, track.y, 20U,
            track.height };
        const struct ui_rect plus = { track.x + track.width, track.y, 20U,
            track.height };
        static const uint32_t notches[7] = { 12U, 25U, 50U, 100U, 200U,
            400U, 800U };
        size_t notch = 0U;

        for (size_t index = 0U; index < 7U; ++index) {
            if (zoom_percent >= notches[index]) {
                notch = index;
            }
        }
        if (holds(track, point)) {
            notch = (size_t)(((uint32_t)point.x - track.x) * 6U +
                track.width / 2U) / track.width;
        } else if (holds(minus, point) && notch > 0U) {
            --notch;
        } else if (holds(plus, point) && notch + 1U < 7U) {
            ++notch;
        } else {
            notch = 7U;
        }
        if (notch < 7U) {
            zoom_percent = notches[notch];
            *damage = window_rect;
            return PAINT_STATUS_OK;
        }
    }
    if (holds(paint_sheet_bounds(), point)) {
        uint32_t x;
        uint32_t y;

        if (!canvas_point(point, &x, &y)) {
            return PAINT_STATUS_OK;
        }
        if (select_mode) {
            selecting = true;
            selection_origin = (struct ui_point){ (int32_t)x, (int32_t)y };
            selection = (struct ui_rect){ x, y, 1U, 1U };
            selection_valid = true;
        } else if (shape_mode) {
            capture_undo();
            shape_dragging = true;
            shape_origin = (struct ui_point){ (int32_t)x, (int32_t)y };
            text_active = false;
        } else if (current_tool == PAINT_TOOL_FILL) {
            const uint32_t colour = pack_rgb(swatches[colour_one]);

            capture_undo();
            flood_fill(x, y, colour);
            image_dirty = true;
        } else if (current_tool == PAINT_TOOL_PICKER) {
            const uint32_t colour = canvas_pixels[
                (size_t)y * PAINT_CANVAS_WIDTH + x];

            for (size_t index = 0U; index < PAINT_SWATCHES; ++index) {
                if (pack_rgb(swatches[index]) == colour) {
                    colour_one = index;
                    break;
                }
            }
        } else if (current_tool == PAINT_TOOL_MAGNIFIER) {
            zoom_percent = zoom_percent >= 400U ? 100U :
                zoom_percent + 25U;
        } else if (current_tool == PAINT_TOOL_TEXT) {
            capture_undo();
            text_active = true;
            text_origin = (struct ui_point){ (int32_t)x, (int32_t)y };
            text_length = 0U;
            text_buffer[0U] = '\0';
        } else if (current_tool == PAINT_TOOL_PENCIL ||
                current_tool == PAINT_TOOL_ERASER) {
            capture_undo();
            painting = true;
            last_paint_point = point;
            paint_dot(point, current_tool == PAINT_TOOL_ERASER ?
                pack_rgb(sheet) : pack_rgb(swatches[colour_one]),
                current_tool == PAINT_TOOL_ERASER ? stroke_radius + 3U :
                    stroke_radius);
            image_dirty = true;
        }
        *damage = window_rect;
        return PAINT_STATUS_OK;
    }
    for (size_t tool = 0U; tool < PAINT_TOOL_COUNT; ++tool) {
        if (holds(tool_rect(tool), point)) {
            current_tool = (enum paint_tool)tool;
            select_mode = false;
            shape_mode = false;
            text_active = false;
            *damage = ribbon_rect();
            return PAINT_STATUS_OK;
        }
    }
    for (size_t shape = 0U; shape < PAINT_SHAPES; ++shape) {
        if (holds(shape_rect(shape), point)) {
            current_shape = shape;
            shape_mode = true;
            select_mode = false;
            text_active = false;
            *damage = ribbon_rect();
            return PAINT_STATUS_OK;
        }
    }
    for (size_t index = 0U; index < PAINT_SWATCHES; ++index) {
        if (holds(swatch_rect(index), point)) {
            if (colour_target == 0U) {
                colour_one = index;
            } else {
                colour_two = index;
            }
            *damage = ribbon_rect();
            return PAINT_STATUS_OK;
        }
    }
    return PAINT_STATUS_OK;
}

enum paint_status paint_pointer_release(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (selecting) {
        uint32_t x;
        uint32_t y;

        if (canvas_point(point, &x, &y)) {
            selection = canvas_rect(selection_origin,
                (struct ui_point){ (int32_t)x, (int32_t)y });
            selection_valid = true;
        }
        selecting = false;
        *damage = paint_sheet_bounds();
    }
    if (shape_dragging) {
        uint32_t x;
        uint32_t y;

        if (canvas_point(point, &x, &y)) {
            paint_shape_between(shape_origin,
                (struct ui_point){ (int32_t)x, (int32_t)y });
            image_dirty = true;
        }
        shape_dragging = false;
        redo_valid = false;
        *damage = paint_sheet_bounds();
    }
    if (painting) {
        image_dirty = true;
        *damage = paint_sheet_bounds();
    }
    painting = false;
    return PAINT_STATUS_OK;
}

static enum paint_status render_text_buffer(void)
{
    if (!text_active || !undo_valid) {
        return PAINT_STATUS_OK;
    }
    copy_canvas(canvas_pixels, undo_pixels);
    if (text_length == 0U) {
        return PAINT_STATUS_OK;
    }
    const struct ui_font_metrics metrics = ui_font_get_metrics();
    uint32_t top = text_origin.y < 0 ? 0U : (uint32_t)text_origin.y;

    if (metrics.height == 0U || top + metrics.height > image_height) {
        return PAINT_STATUS_BAD_INDEX;
    }
    struct surface target = {
        .active = true,
        .width = PAINT_CANVAS_WIDTH,
        .height = PAINT_CANVAS_HEIGHT,
        .pitch = PAINT_CANVAS_WIDTH * SURFACE_BYTES_PER_PIXEL,
        .pixels = canvas_pixels
    };
    const struct surface_rect bounds = { 0U, 0U, image_width, image_height };

    return ui_font_draw_text(&target, bounds,
        text_origin.x < 0 ? 0U : (uint32_t)text_origin.x,
        top + metrics.ascent, text_buffer,
        pack_rgb(swatches[colour_one]), NULL) == UI_FONT_STATUS_OK ?
            PAINT_STATUS_OK : PAINT_STATUS_SURFACE_FAILURE;
}

enum paint_status paint_text_input(char character, struct ui_rect *damage)
{
    if (damage == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (resize_dialog_open) {
        const uint32_t old_width = resize_width_value;
        const uint32_t old_height = resize_height_value;
        uint32_t *const field = resize_field == 0U ? &resize_width_value :
            &resize_height_value;
        const uint32_t limit = resize_field == 0U ? PAINT_CANVAS_WIDTH :
            PAINT_CANVAS_HEIGHT;

        if (character >= '0' && character <= '9') {
            const uint32_t candidate = *field * 10U +
                (uint32_t)(character - '0');

            if (candidate <= limit) {
                *field = candidate;
                if (!resize_sync_other()) {
                    resize_width_value = old_width;
                    resize_height_value = old_height;
                }
            }
        }
        *damage = resize_dialog_rect();
        return PAINT_STATUS_OK;
    }
    if (!text_active || character < ' ' || character > '~') {
        return PAINT_STATUS_OK;
    }
    if (text_length + 1U >= PAINT_TEXT_BYTES) {
        return PAINT_STATUS_BAD_INDEX;
    }
    uint32_t text_width = 0U;
    char prospective[PAINT_TEXT_BYTES];

    for (size_t index = 0U; index < text_length; ++index) {
        prospective[index] = text_buffer[index];
    }
    prospective[text_length] = character;
    prospective[text_length + 1U] = '\0';
    if (ui_font_text_width(prospective, &text_width) != UI_FONT_STATUS_OK ||
            text_origin.x < 0 || (uint32_t)text_origin.x + text_width >
                image_width) {
        return PAINT_STATUS_BAD_INDEX;
    }
    text_buffer[text_length++] = character;
    text_buffer[text_length] = '\0';
    const enum paint_status status = render_text_buffer();

    if (status == PAINT_STATUS_OK) {
        image_dirty = true;
        *damage = paint_sheet_bounds();
    }
    return status;
}

enum paint_status paint_key_backspace(struct ui_rect *damage)
{
    if (damage == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (resize_dialog_open) {
        const uint32_t old_width = resize_width_value;
        const uint32_t old_height = resize_height_value;
        uint32_t *const field = resize_field == 0U ? &resize_width_value :
            &resize_height_value;

        *field /= 10U;
        if (!resize_sync_other()) {
            resize_width_value = old_width;
            resize_height_value = old_height;
        }
        *damage = resize_dialog_rect();
        return PAINT_STATUS_OK;
    }
    if (!text_active || text_length == 0U) {
        return PAINT_STATUS_OK;
    }
    text_buffer[--text_length] = '\0';
    const enum paint_status status = render_text_buffer();

    if (status == PAINT_STATUS_OK) {
        image_dirty = true;
        *damage = paint_sheet_bounds();
    }
    return status;
}

enum paint_status paint_key_enter(struct ui_rect *damage)
{
    if (damage == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (resize_dialog_open) {
        const enum paint_status status = apply_resize_dialog();

        *damage = status == PAINT_STATUS_OK ? window_rect :
            resize_dialog_rect();
        return status;
    }
    if (!text_active) {
        return PAINT_STATUS_OK;
    }
    const struct ui_font_metrics metrics = ui_font_get_metrics();
    const uint32_t next = (uint32_t)text_origin.y + metrics.height;

    capture_undo();
    text_origin.y = (int32_t)(next < image_height ? next :
        image_height - 1U);
    text_length = 0U;
    text_buffer[0U] = '\0';
    *damage = paint_sheet_bounds();
    return PAINT_STATUS_OK;
}

bool paint_take_save_request(void)
{
    const bool requested = save_requested;

    save_requested = false;
    return requested;
}

void paint_mark_saved(void)
{
    image_dirty = false;
    (void)paint_set_title("PAINT.BMP - Paint");
}

struct paint_image_info paint_image(void)
{
    return (struct paint_image_info){ image_width, image_height,
        (image_width * 3U + 3U) & ~UINT32_C(3), image_dirty };
}

enum paint_status paint_copy_bgr24_row(uint32_t row, uint8_t *destination,
    size_t capacity, size_t *written)
{
    if (destination == NULL || written == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    const struct paint_image_info image = paint_image();

    if (row >= image.height || capacity < image.row_stride) {
        return PAINT_STATUS_BAD_INDEX;
    }
    const struct framebuffer_state format = framebuffer_get_state();

    for (uint32_t x = 0U; x < image.row_stride; ++x) {
        destination[x] = 0U;
    }
    for (uint32_t x = 0U; x < image.width; ++x) {
        const uint32_t pixel = canvas_pixels[
            (size_t)row * PAINT_CANVAS_WIDTH + x];
        const size_t at = (size_t)x * 3U;

        destination[at] = (uint8_t)(pixel >> format.blue_position);
        destination[at + 1U] = (uint8_t)(pixel >> format.green_position);
        destination[at + 2U] = (uint8_t)(pixel >> format.red_position);
    }
    *written = image.row_stride;
    return PAINT_STATUS_OK;
}

/* ============================================================== LIFECYCLE */

enum paint_status paint_set_frame(struct ui_rect frame)
{
    const uint32_t needed = PAINT_BORDER * 2U + 640U;
    const uint32_t least_height = PAINT_BORDER * 2U + PAINT_CAPTION +
        PAINT_TABS + PAINT_RIBBON + PAINT_STATUS + 80U;

    /* The ribbon is deliberately clipped by the window at narrow desktop
     * sizes.  Requiring the sum of every ribbon group made Paint impossible
     * to open on Phipia's supported 1024x768 framebuffer even though its
     * canvas and the visible groups fit comfortably. */
    if (frame.width < needed || frame.height < least_height) {
        return PAINT_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return PAINT_STATUS_OK;
}

enum paint_status paint_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum paint_status status;

    if (target == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    canvas = target;
    status = paint_set_frame(frame);
    if (status != PAINT_STATUS_OK) {
        return status;
    }
    if (!allocate_editing_buffers()) {
        return PAINT_STATUS_ALLOCATION_FAILURE;
    }
    for (size_t index = 0U;
         index < (size_t)PAINT_CANVAS_PIXELS; ++index) {
        canvas_pixels[index] = pack_rgb(sheet);
    }
    image_width = PAINT_CANVAS_WIDTH;
    image_height = PAINT_CANVAS_HEIGHT;
    undo_valid = false;
    redo_valid = false;
    clipboard_valid = false;
    image_dirty = false;
    save_requested = false;
    painting = false;
    selecting = false;
    select_mode = false;
    selection_valid = false;
    shape_mode = false;
    shape_dragging = false;
    text_active = false;
    text_length = 0U;
    text_buffer[0U] = '\0';
    resize_dialog_open = false;
    resize_keep_aspect = true;
    resize_field = 0U;
    resize_width_value = image_width;
    resize_height_value = image_height;
    initialized = true;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_tool(enum paint_tool tool)
{
    if ((size_t)tool >= PAINT_TOOL_COUNT) {
        return PAINT_STATUS_BAD_INDEX;
    }
    current_tool = tool;
    select_mode = false;
    shape_mode = false;
    text_active = false;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_shape(size_t shape)
{
    if (shape >= PAINT_SHAPES) {
        return PAINT_STATUS_BAD_INDEX;
    }
    current_shape = shape;
    shape_mode = true;
    select_mode = false;
    text_active = false;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_colours(size_t first, size_t second)
{
    if (first >= PAINT_SWATCHES || second >= PAINT_SWATCHES) {
        return PAINT_STATUS_BAD_INDEX;
    }
    colour_one = first;
    colour_two = second;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_zoom(uint32_t percent)
{
    if (percent == 0U || percent > 800U) {
        return PAINT_STATUS_BAD_INDEX;
    }
    zoom_percent = percent;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_title(const char *title)
{
    size_t index = 0U;

    if (title == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    while (index + 1U < sizeof(window_title) && title[index] != '\0') {
        window_title[index] = title[index];
        ++index;
    }
    window_title[index] = '\0';
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_focus(bool active)
{
    focused = active;
    return PAINT_STATUS_OK;
}

/* ============================================================== SELF TEST */

bool paint_self_test(void)
{
    uint32_t size = 0U;

    /*
     * Paint's palette is Paint's, not a set of colours that look about
     * right.  These four are the ones anybody would recognise, and if the
     * table has been edited they are the first to move.
     */
    if (swatches[3].red != 0xEDU || swatches[3].green != 0x1CU ||
            swatches[3].blue != 0x24U) {
        self_test_failure = "Paint's red is not ED1C24";
        return false;
    }
    if (swatches[7].red != 0x00U || swatches[7].green != 0xA2U ||
            swatches[7].blue != 0xE8U) {
        self_test_failure = "Paint's turquoise is not 00A2E8";
        return false;
    }
    if (swatches[10].red != 0xFFU || swatches[10].green != 0xFFU ||
            swatches[10].blue != 0xFFU) {
        self_test_failure = "Paint's eleventh swatch is not white";
        return false;
    }
    if (swatches[12].red != 0xB9U || swatches[12].green != 0x7AU ||
            swatches[12].blue != 0x57U) {
        self_test_failure = "Paint's brown is not B97A57";
        return false;
    }
    /* Every tool and every shape has a mark, or a cell comes out blank. */
    for (size_t tool = 0U; tool < PAINT_TOOL_COUNT; ++tool) {
        if (glyph_cell(tool_glyph(tool), PAINT_TOOL_CELL, &size) == NULL) {
            self_test_failure = "a Paint tool has no icon";
            return false;
        }
    }
    for (size_t shape = 0U; shape < PAINT_SHAPES; ++shape) {
        if (glyph_cell(shape_glyph[shape], PAINT_SHAPE_CELL, &size) ==
                NULL) {
            self_test_failure = "a Paint shape has no icon";
            return false;
        }
    }
    /* The tool grid is three across and two down, and holds all six. */
    if (PAINT_TOOL_COLUMNS * PAINT_TOOL_ROWS != PAINT_TOOL_COUNT) {
        self_test_failure = "Paint's tool grid does not hold its six tools";
        return false;
    }
    if (PAINT_SWATCH_COLUMNS * 2U != PAINT_SWATCHES) {
        self_test_failure = "Paint's palette is not two rows of ten";
        return false;
    }
    /* The default zoom has to land in the middle of its own track, which is
     * the whole point of the notches being doublings. */
    {
        static const uint32_t notches[7] = { 12U, 25U, 50U, 100U, 200U,
            400U, 800U };
        size_t notch = 0U;

        for (size_t index = 0U; index < 7U; ++index) {
            if (100U >= notches[index]) {
                notch = index;
            }
        }
        if (notch != 3U) {
            self_test_failure = "Paint's 100% zoom is not the middle notch";
            return false;
        }
    }
    if (paint_set_tool((enum paint_tool)PAINT_TOOL_COUNT) !=
            PAINT_STATUS_BAD_INDEX ||
            paint_set_shape(PAINT_SHAPES) != PAINT_STATUS_BAD_INDEX ||
            paint_set_colours(PAINT_SWATCHES, 0U) !=
                PAINT_STATUS_BAD_INDEX ||
            paint_set_zoom(0U) != PAINT_STATUS_BAD_INDEX) {
        self_test_failure = "Paint accepted a value off the end of a table";
        return false;
    }
    /* A group narrower than the word beneath it centres that word by
     * overflowing to the right, onto the divider of the group beside it.
     * That is how the Brushes group used to read as smudged. */
    for (enum paint_group group = PAINT_GROUP_CLIPBOARD;
         group < PAINT_GROUP_COUNT; group = (enum paint_group)(group + 1)) {
        if (width_of(group_label(group)) + 6U > group_width(group)) {
            self_test_failure =
                "a Paint ribbon group is narrower than its own label";
            return false;
        }
    }
    self_test_failure = "";
    return true;
}

const char *paint_self_test_failure(void)
{
    return self_test_failure;
}
