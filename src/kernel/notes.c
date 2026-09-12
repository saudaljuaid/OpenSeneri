/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Notes.  See include/phipia/notes.h for what this is and why it is not just
 * a copy of Sticky Notes.
 */

#include <phipia/notes.h>

#include <phipia/cursor.h>

#include <phipia/framebuffer.h>
#include <phipia/ui_font.h>

#include "notes_glyphs.h"
#include "notes_marks.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION: read off a Windows 10 Sticky Notes window at 100%
 * scaling, with the two-pane layout's own numbers chosen to match it.
 */

/* Sticky Notes' header strip is the note's colour, a shade darker than the
 * pad, with the plus at one end and the overflow dots at the other. */
#define NOTES_HEADER_HEIGHT 40U
#define NOTES_BORDER 1U
/* The list pane, which Sticky Notes puts in a separate window. */
#define NOTES_LIST_WIDTH 220U
#define NOTES_LIST_ROW 64U
#define NOTES_LIST_PAD 12U
/* The accent bar Windows draws down the left edge of a selected list row. */
#define NOTES_SELECTION_BAR 3U
#define NOTES_SELECTION_INSET 10U
/* The editor. */
#define NOTES_TEXT_INSET 20U
#define NOTES_LINE_HEIGHT 28U
#define NOTES_CHECK_BOX 16U
#define NOTES_CHECK_GAP 12U
#define NOTES_TITLE_HEIGHT 44U
/* The formatting bar along the bottom, which is Sticky Notes' own. */
#define NOTES_TOOLBAR_HEIGHT 40U
#define NOTES_TOOL_SIZE 32U
#define NOTES_TOOL_COUNT 5U
#define NOTES_TOOL_GLYPH 20U

/* ================================================================ PALETTE
 *
 * Sticky Notes' five pads.  Each is a body and a header a shade deeper, and
 * the pair is what makes a note read as paper rather than as a panel.
 * PENDING VERIFICATION.
 */
struct notes_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define NOTES_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

struct notes_pad {
    struct notes_rgb body;
    struct notes_rgb header;
    struct notes_rgb accent;
};

static const struct notes_pad pads[NOTES_COLOUR_COUNT] = {
    { NOTES_RGB(0xFEU, 0xEF, 0x86U), NOTES_RGB(0xFDU, 0xE3U, 0x4FU),
      NOTES_RGB(0xE8U, 0xA4U, 0x14U) },   /* yellow, the default pad */
    { NOTES_RGB(0xD3U, 0xF2U, 0xC4U), NOTES_RGB(0xB6U, 0xE8U, 0x9EU),
      NOTES_RGB(0x3FU, 0x9AU, 0x2EU) },   /* green  */
    { NOTES_RGB(0xFBU, 0xD5U, 0xE4U), NOTES_RGB(0xF7U, 0xB6U, 0xCFU),
      NOTES_RGB(0xC2U, 0x3F, 0x7BU) },    /* pink   */
    { NOTES_RGB(0xE2U, 0xD8U, 0xF3U), NOTES_RGB(0xCBU, 0xB8U, 0xE9U),
      NOTES_RGB(0x6BU, 0x3FU, 0xB5U) },   /* purple */
    { NOTES_RGB(0xCFU, 0xE8U, 0xFAU), NOTES_RGB(0xA9U, 0xD7U, 0xF6U),
      NOTES_RGB(0x0FU, 0x6CU, 0xBDU) }    /* blue   */
};

/* The window's own chrome, which is not the pad's colour: the list pane is a
 * near-white surface and the window border is Windows' accent. */
static const struct notes_rgb list_background = NOTES_RGB(0xFAU, 0xFAU, 0xFAU);
static const struct notes_rgb list_hover = NOTES_RGB(0xF0U, 0xF0U, 0xF0U);
static const struct notes_rgb list_selected = NOTES_RGB(0xE8U, 0xE8U, 0xE8U);
static const struct notes_rgb ink = NOTES_RGB(0x1AU, 0x1AU, 0x1AU);
static const struct notes_rgb ink_soft = NOTES_RGB(0x60U, 0x60U, 0x60U);
static const struct notes_rgb ink_done = NOTES_RGB(0x9AU, 0x9AU, 0x9AU);
static const struct notes_rgb border_active = NOTES_RGB(0x00U, 0x78U, 0xD7U);
static const struct notes_rgb border_inactive = NOTES_RGB(0x9BU, 0x9BU,
    0x9BU);

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool focused = true;
static struct notes_note notes[NOTES_MAX_NOTES];
static size_t selected;
static size_t editing_line = (size_t)-1;
static size_t hover_note = (size_t)-1;
static size_t hover_line = (size_t)-1;
static const char *self_test_failure = "notes self-test has not run";

const char *notes_status_string(enum notes_status status)
{
    switch (status) {
    case NOTES_STATUS_OK:
        return "ok";
    case NOTES_STATUS_NULL_ARGUMENT:
        return "null argument";
    case NOTES_STATUS_NOT_INITIALIZED:
        return "notes not initialized";
    case NOTES_STATUS_BAD_INDEX:
        return "notes index is out of range";
    case NOTES_STATUS_UNSUPPORTED_GEOMETRY:
        return "notes geometry is unsupported";
    case NOTES_STATUS_SURFACE_FAILURE:
        return "notes surface refused a pixel";
    default:
        return "unknown notes status";
    }
}

/* ================================================================ DRAWING */

static uint32_t pack_rgb(struct notes_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return ((uint32_t)colour.red << format.red_position) |
        ((uint32_t)colour.green << format.green_position) |
        ((uint32_t)colour.blue << format.blue_position);
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

static enum notes_status fill(struct ui_rect area, struct ui_rect damage,
    struct notes_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return NOTES_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return NOTES_STATUS_OK;
}

static enum notes_status text(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct notes_rgb colour)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (clip.width == 0U || clip.height == 0U) {
        return NOTES_STATUS_OK;
    }
    bounds.x = window_rect.x;
    bounds.y = window_rect.y;
    bounds.width = window_rect.width;
    bounds.height = window_rect.height;
    region.x = clip.x;
    region.y = clip.y;
    region.width = clip.width;
    region.height = clip.height;
    if (ui_font_draw_text_clipped(canvas, bounds, region, x, baseline, body,
            pack_rgb(colour), NULL) != UI_FONT_STATUS_OK) {
        return NOTES_STATUS_OK;   /* a glyph the font lacks is not a failure */
    }
    return NOTES_STATUS_OK;
}

static enum notes_status styled_text(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct notes_rgb colour,
    uint32_t style)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    const struct surface_rect bounds = {
        window_rect.x, window_rect.y, window_rect.width, window_rect.height
    };
    const struct surface_rect region = {
        clip.x, clip.y, clip.width, clip.height
    };

    if (clip.width == 0U || clip.height == 0U) {
        return NOTES_STATUS_OK;
    }
    return ui_font_draw_text_styled_clipped(canvas, bounds, region, x,
        baseline, body, pack_rgb(colour), style, NULL) == UI_FONT_STATUS_OK ?
            NOTES_STATUS_OK : NOTES_STATUS_SURFACE_FAILURE;
}

static uint32_t text_width_of(const char *body)
{
    uint32_t width = 0U;

    if (ui_font_text_width(body, &width) != UI_FONT_STATUS_OK) {
        return 0U;
    }
    return width;
}

/* --- geometry --- */

static struct ui_rect list_rect(void)
{
    return (struct ui_rect){
        window_rect.x + NOTES_BORDER,
        window_rect.y + NOTES_BORDER,
        NOTES_LIST_WIDTH,
        window_rect.height - NOTES_BORDER * 2U
    };
}

static struct ui_rect pad_rect(void)
{
    const struct ui_rect list = list_rect();

    return (struct ui_rect){
        list.x + list.width,
        window_rect.y + NOTES_BORDER,
        window_rect.width - NOTES_BORDER * 2U - list.width,
        window_rect.height - NOTES_BORDER * 2U
    };
}

/* Which of the present notes this index is, counting from the top. */
static size_t list_position(size_t index)
{
    size_t position = 0U;

    for (size_t scan = 0U; scan < index; ++scan) {
        if (notes[scan].present) {
            ++position;
        }
    }
    return position;
}

static struct ui_rect list_row_rect(size_t index)
{
    const struct ui_rect list = list_rect();
    const uint32_t top = list.y + NOTES_HEADER_HEIGHT +
        (uint32_t)list_position(index) * NOTES_LIST_ROW;

    if (!notes[index].present || top + NOTES_LIST_ROW > list.y + list.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ list.x, top, list.width, NOTES_LIST_ROW };
}

/* Which of the present lines this index is, counting from the top. */
static size_t line_position(size_t note, size_t index)
{
    size_t position = 0U;

    for (size_t scan = 0U; scan < index; ++scan) {
        if (notes[note].lines[scan].present) {
            ++position;
        }
    }
    return position;
}

static struct ui_rect line_rect(size_t index)
{
    const struct ui_rect pad = pad_rect();
    const uint32_t top = pad.y + NOTES_HEADER_HEIGHT + NOTES_TITLE_HEIGHT +
        (uint32_t)line_position(selected, index) * NOTES_LINE_HEIGHT;
    const uint32_t floor = pad.y + pad.height - NOTES_TOOLBAR_HEIGHT;

    if (!notes[selected].lines[index].present ||
            top + NOTES_LINE_HEIGHT > floor) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ pad.x + NOTES_TEXT_INSET, top,
        pad.width - NOTES_TEXT_INSET * 2U, NOTES_LINE_HEIGHT };
}

static struct ui_rect check_rect(size_t index)
{
    const struct ui_rect line = line_rect(index);

    if (line.width == 0U) {
        return line;
    }
    return (struct ui_rect){ line.x,
        line.y + (line.height - NOTES_CHECK_BOX) / 2U,
        NOTES_CHECK_BOX, NOTES_CHECK_BOX };
}

static struct ui_rect new_note_rect(void)
{
    const struct ui_rect list = list_rect();

    return (struct ui_rect){ list.x + 6U, list.y + 4U, 40U,
        NOTES_HEADER_HEIGHT - 8U };
}

/*
 * A caret over the pad, an arrow over the list beside it.  The pad is where
 * the words are; the list is a column of buttons that pick which note the
 * pad is showing, and a caret over a button would be a promise to type
 * into one.
 */
enum cursor_kind notes_cursor_at(struct ui_point point)
{
    if (!initialized || !holds(window_rect, point)) {
        return CURSOR_NORMAL_SELECT;
    }
    if (holds(pad_rect(), point)) {
        return CURSOR_TEXT_SELECT;
    }
    return CURSOR_NORMAL_SELECT;
}

struct ui_rect notes_bounds(void)
{
    return window_rect;
}

/* --- pieces --- */

/*
 * A tick, drawn as two strokes rather than as a glyph.
 *
 * The mark is eleven pixels across and its short arm is two fifths of its
 * long one, which is the proportion the note icon on the taskbar uses; a tick
 * with equal arms reads as a check mark from a spreadsheet.
 */
static enum notes_status draw_tick(struct ui_rect box, struct ui_rect damage,
    struct notes_rgb colour)
{
    const uint32_t weight = 2U;
    const uint32_t short_arm = box.width / 5U;
    const uint32_t long_arm = box.width * 7U / 16U;
    /*
     * The corner the two arms meet at, derived so the MARK is centred rather
     * than the corner.
     *
     * The ink runs from pivot_x - short_arm to pivot_x + long_arm + weight
     * across, and from pivot_y - long_arm to pivot_y + weight down, so its
     * extent is known before it is drawn and the pivot can be placed to
     * centre it.  Positioning the pivot instead - which is what this did -
     * put the long arm flush against the right edge with four pixels of air
     * on the left, and no size of box made it come out right.
     */
    const uint32_t ink_width = short_arm + long_arm + weight;
    const uint32_t ink_height = long_arm + weight;
    const uint32_t pivot_x = box.x + short_arm +
        (box.width > ink_width ? (box.width - ink_width) / 2U : 0U);
    const uint32_t pivot_y = box.y + long_arm +
        (box.height > ink_height ? (box.height - ink_height) / 2U : 0U);
    enum notes_status status = NOTES_STATUS_OK;

    if (box.width < 8U || short_arm == 0U || pivot_x < box.x + short_arm) {
        return NOTES_STATUS_OK;
    }
    for (uint32_t step = 0U; step <= short_arm && status == NOTES_STATUS_OK;
         ++step) {
        status = fill((struct ui_rect){
            pivot_x - short_arm + step, pivot_y - short_arm + step,
            weight, weight }, damage, colour);
    }
    for (uint32_t step = 1U; step <= long_arm && status == NOTES_STATUS_OK;
         ++step) {
        status = fill((struct ui_rect){
            pivot_x + step, pivot_y - step, weight, weight }, damage,
            colour);
    }
    return status;
}

static enum notes_status draw_check_box(size_t index, struct ui_rect damage,
    const struct notes_pad *pad)
{
    const struct ui_rect box = check_rect(index);
    const struct notes_line *line = &notes[selected].lines[index];
    enum notes_status status;

    if (box.width == 0U) {
        return NOTES_STATUS_OK;
    }
    if (line->done) {
        /* A finished item fills its box with the pad's own accent and puts a
         * white tick in it, which is how every Windows check box reads. */
        status = fill(box, damage, pad->accent);
        if (status != NOTES_STATUS_OK) {
            return status;
        }
        return draw_tick(box, damage, (struct notes_rgb)NOTES_RGB(0xFFU,
            0xFFU, 0xFFU));
    }
    /* An unfinished one is an empty two-pixel outline. */
    status = fill((struct ui_rect){ box.x, box.y, box.width, 2U }, damage,
        ink_soft);
    if (status == NOTES_STATUS_OK) {
        status = fill((struct ui_rect){ box.x, box.y + box.height - 2U,
            box.width, 2U }, damage, ink_soft);
    }
    if (status == NOTES_STATUS_OK) {
        status = fill((struct ui_rect){ box.x, box.y, 2U, box.height },
            damage, ink_soft);
    }
    if (status == NOTES_STATUS_OK) {
        status = fill((struct ui_rect){ box.x + box.width - 2U, box.y, 2U,
            box.height }, damage, ink_soft);
    }
    return status;
}

/*
 * The header strip's two controls: a plus at the left and the overflow dots
 * at the right, which is where Sticky Notes puts them.
 */
static enum notes_status draw_header_plus(struct ui_rect strip,
    struct ui_rect damage, struct notes_rgb colour)
{
    const uint32_t arm = 14U;
    const uint32_t weight = 2U;
    const uint32_t centre_y = strip.y + strip.height / 2U;
    const uint32_t plus_x = strip.x + 16U;
    const enum notes_status status = fill((struct ui_rect){
        plus_x, centre_y - weight / 2U, arm, weight }, damage, colour);

    if (status != NOTES_STATUS_OK) {
        return status;
    }
    return fill((struct ui_rect){ plus_x + arm / 2U - weight / 2U,
        centre_y - arm / 2U, weight, arm }, damage, colour);
}

/*
 * The overflow dots and the close cross, at the right-hand end of the pad's
 * strip in that order - which is where Sticky Notes puts them, and it is the
 * only chrome the window has: the coloured strip IS the title bar.
 */
static enum notes_status draw_header_overflow(struct ui_rect strip,
    struct ui_rect damage, struct notes_rgb colour)
{
    const uint32_t centre_y = strip.y + strip.height / 2U;
    const uint32_t cross = 10U;
    const uint32_t controls_x = strip.x + strip.width - 138U;
    const uint32_t cross_x = controls_x + 92U + 18U;
    const uint32_t cross_y = centre_y - cross / 2U;
    enum notes_status status = NOTES_STATUS_OK;

    status = fill((struct ui_rect){ controls_x + 16U, centre_y + 5U,
        14U, 1U }, damage, colour);
    if (status == NOTES_STATUS_OK) {
        status = fill((struct ui_rect){ controls_x + 46U + 16U,
            centre_y - 7U, 14U, 14U }, damage, colour);
    }
    if (status == NOTES_STATUS_OK) {
        status = fill((struct ui_rect){ controls_x + 46U + 18U,
            centre_y - 5U, 10U, 10U }, damage,
            pads[notes[selected].colour].header);
    }
    for (uint32_t step = 0U; step < cross && status == NOTES_STATUS_OK;
         ++step) {
        status = fill((struct ui_rect){ cross_x + step, cross_y + step, 1U,
            1U }, damage, colour);
        if (status == NOTES_STATUS_OK) {
            status = fill((struct ui_rect){ cross_x + step,
                cross_y + cross - 1U - step, 1U, 1U }, damage, colour);
        }
    }
    return status;
}

/* One alpha cell, composited flat in a colour, the way every mark here is. */
static enum notes_status draw_glyph_cell(const uint8_t *cell, uint32_t size,
    struct ui_rect box, struct ui_rect damage, struct notes_rgb colour)
{
    const struct ui_rect clipped = intersect(box, damage);
    const uint32_t over = pack_rgb(colour);
    const struct framebuffer_state format = framebuffer_get_state();

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - box.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - box.x + x;
            const uint8_t coverage = cell[local_y * size + local_x];
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (coverage == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return NOTES_STATUS_SURFACE_FAILURE;
            }
            red = ((uint32_t)((over >> format.red_position) & 0xFFU) *
                coverage + ((under >> format.red_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            green = ((uint32_t)((over >> format.green_position) & 0xFFU) *
                coverage + ((under >> format.green_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            blue = ((uint32_t)((over >> format.blue_position) & 0xFFU) *
                coverage + ((under >> format.blue_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) != SURFACE_STATUS_OK) {
                return NOTES_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return NOTES_STATUS_OK;
}

static const uint8_t *notes_glyph(const char *name, uint32_t wanted,
    uint32_t *size)
{
    size_t choice = NOTES_LUCIDE_SIZES - 1U;

    for (size_t index = 0U; index < NOTES_LUCIDE_COUNT; ++index) {
        const char *left = notes_lucide[index].name;
        const char *right = name;

        while (*left != '\0' && *left == *right) {
            ++left;
            ++right;
        }
        if (*left != '\0' || *right != '\0') {
            continue;
        }
        for (size_t option = 0U; option < NOTES_LUCIDE_SIZES; ++option) {
            if (notes_lucide_size[option] >= wanted) {
                choice = option;
                break;
            }
        }
        *size = notes_lucide_size[choice];
        return notes_lucide[index].alpha[choice];
    }
    return NULL;
}

/*
 * The formatting bar.
 *
 * Bold, italic, underline, strikethrough and a list, which is what Sticky
 * Notes puts along the bottom of a pad.
 *
 * The first four are TYPE, not icons, and they are set in the cut that means
 * them - B from Inter Bold, I from Inter Italic, U and S from Inter Regular,
 * the same family as the body text.  They were Lucide's marks for the same
 * four and they looked broken for a reason worth keeping: Lucide is
 * stroke-only, so its `bold` is an OUTLINE of a letter B, and at sixteen
 * pixels a two-unit stroke on a 24-unit grid hints down to a single pixel.
 * A bold button drawn as a hairline outline is self-contradictory, and its
 * `strikethrough` - an S whose bowl is one pixel wide with a rule through it
 * - reads as a glyph that failed to load.
 *
 * Each mark is cropped to its own ink and carries that ink's size and
 * baseline, so this centres what is actually drawn.  Centring the cell
 * instead sits all four too high, because a cell reserves descender space
 * and none of B, I, U or S has a descender.
 *
 * The list stays an icon, because a list IS a picture of a list.  It is
 * Lucide's `list` rather than `list-todo`: the todo variant carries a tiny
 * check box that collapses into a blob at this size, which is why that one
 * mark had two different stroke weights in it.
 */
enum notes_tool_style {
    NOTES_TOOL_BOLD = 0,
    NOTES_TOOL_ITALIC,
    NOTES_TOOL_UNDERLINE,
    NOTES_TOOL_STRIKE,
    NOTES_TOOL_LIST
};

static struct ui_rect toolbar_slot(size_t tool)
{
    const struct ui_rect area = pad_rect();
    const struct ui_rect bar = {
        area.x, area.y + area.height - NOTES_TOOLBAR_HEIGHT,
        area.width, NOTES_TOOLBAR_HEIGHT
    };

    return (struct ui_rect){
        bar.x + NOTES_TEXT_INSET + (uint32_t)tool * NOTES_TOOL_SIZE,
        bar.y + (bar.height - NOTES_TOOL_SIZE) / 2U,
        NOTES_TOOL_SIZE, NOTES_TOOL_SIZE
    };
}

static bool toolbar_tool_active(size_t tool)
{
    if (editing_line >= NOTES_MAX_LINES ||
            !notes[selected].lines[editing_line].present) {
        return false;
    }
    const struct notes_line *line = &notes[selected].lines[editing_line];

    if (tool == NOTES_TOOL_BOLD) {
        return line->bold;
    }
    if (tool == NOTES_TOOL_ITALIC) {
        return line->italic;
    }
    if (tool == NOTES_TOOL_UNDERLINE) {
        return line->underline;
    }
    if (tool == NOTES_TOOL_STRIKE) {
        return line->strike;
    }
    return line->checkable;
}

static const struct notes_mark *notes_mark_named(const char *name)
{
    for (size_t index = 0U; index < NOTES_MARK_COUNT; ++index) {
        const char *left = notes_marks[index].name;
        const char *right = name;

        while (*left != '\0' && *left == *right) {
            ++left;
            ++right;
        }
        if (*left == '\0' && *right == '\0') {
            return &notes_marks[index];
        }
    }
    return NULL;
}

/* One mark's ink, centred in a box.  Returns where its baseline landed, so
 * a rule can be placed against it. */
static enum notes_status draw_mark(const struct notes_mark *mark,
    struct ui_rect box, struct ui_rect damage, struct notes_rgb colour,
    uint32_t *ink_left, uint32_t *ink_width, uint32_t *baseline_y,
    uint32_t *middle_y)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t over = pack_rgb(colour);
    const uint32_t left = box.x + (box.width > mark->width ?
        (box.width - mark->width) / 2U : 0U);
    const uint32_t top = box.y + (box.height > mark->height ?
        (box.height - mark->height) / 2U : 0U);

    *ink_left = left;
    *ink_width = mark->width;
    *baseline_y = top + mark->baseline;
    *middle_y = top + mark->height / 2U;
    for (uint32_t row = 0U; row < mark->height; ++row) {
        for (uint32_t column = 0U; column < mark->width; ++column) {
            const uint8_t coverage = mark->alpha[(size_t)row * mark->width +
                column];
            const uint32_t x = left + column;
            const uint32_t y = top + row;
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (coverage == 0U || x < damage.x ||
                    x >= damage.x + damage.width || y < damage.y ||
                    y >= damage.y + damage.height) {
                continue;
            }
            if (surface_read_pixel(canvas, x, y, &under) !=
                    SURFACE_STATUS_OK) {
                return NOTES_STATUS_SURFACE_FAILURE;
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
            if (surface_pixel(canvas, x, y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) != SURFACE_STATUS_OK) {
                return NOTES_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return NOTES_STATUS_OK;
}

static enum notes_status draw_toolbar(struct ui_rect damage,
    const struct notes_pad *pad)
{
    static const char *const marks[NOTES_TOOL_COUNT] = {
        "bold", "italic", "underline", "strikethrough", NULL
    };
    const struct ui_rect area = pad_rect();
    const struct ui_rect bar = {
        area.x, area.y + area.height - NOTES_TOOLBAR_HEIGHT,
        area.width, NOTES_TOOLBAR_HEIGHT
    };
    enum notes_status status = fill(bar, damage, pad->header);

    for (uint32_t tool = 0U; tool < NOTES_TOOL_COUNT &&
            status == NOTES_STATUS_OK; ++tool) {
        const struct ui_rect slot = toolbar_slot(tool);
        const struct notes_mark *mark = marks[tool] == NULL ? NULL :
            notes_mark_named(marks[tool]);
        uint32_t ink_left = 0U;
        uint32_t ink_width = 0U;
        uint32_t baseline_y = 0U;
        uint32_t middle_y = 0U;

        if (toolbar_tool_active(tool)) {
            status = fill(slot, damage, pad->accent);
        }
        if (status != NOTES_STATUS_OK) {
            return status;
        }
        if (mark == NULL) {
            uint32_t size = 0U;
            const uint8_t *cell = notes_glyph("list", NOTES_TOOL_GLYPH,
                &size);

            if (cell != NULL) {
                status = draw_glyph_cell(cell, size, (struct ui_rect){
                    slot.x + (slot.width - size) / 2U,
                    slot.y + (slot.height - size) / 2U, size, size },
                    damage, ink);
            }
            continue;
        }
        status = draw_mark(mark, slot, damage, ink, &ink_left, &ink_width,
            &baseline_y, &middle_y);
        if (status != NOTES_STATUS_OK) {
            return status;
        }
        /*
         * The rules, placed against the letter rather than against the
         * slot: an underline sits two pixels under the BASELINE and a
         * strike crosses the middle of the INK, and both run a pixel past
         * the letter on each side, which is what a text engine does.
         */
        if (tool == NOTES_TOOL_UNDERLINE) {
            status = fill((struct ui_rect){ ink_left - 1U, baseline_y + 2U,
                ink_width + 2U, 1U }, damage, ink);
        } else if (tool == NOTES_TOOL_STRIKE) {
            status = fill((struct ui_rect){ ink_left - 1U, middle_y,
                ink_width + 2U, 1U }, damage, ink);
        }
    }
    return status;
}

static enum notes_status draw_list(struct ui_rect damage)
{
    const struct ui_rect list = list_rect();
    const struct notes_pad *chrome = &pads[notes[selected].colour];
    enum notes_status status = fill(list, damage, list_background);

    if (status != NOTES_STATUS_OK) {
        return status;
    }
    /* The list's own header strip, in the SELECTED note's colour, so the two
     * panes read as one window rather than as two. */
    status = fill((struct ui_rect){ list.x, list.y, list.width,
        NOTES_HEADER_HEIGHT }, damage, chrome->header);
    /* The strip carries ONE control each: new-note on the list, overflow on
     * the pad.  Both on both is two pluses and two menus, which is what it
     * looked like before. */
    if (status == NOTES_STATUS_OK) {
        status = draw_header_plus((struct ui_rect){ list.x, list.y,
            list.width, NOTES_HEADER_HEIGHT }, damage, ink);
    }
    for (size_t index = 0U; index < NOTES_MAX_NOTES &&
            status == NOTES_STATUS_OK; ++index) {
        const struct ui_rect row = list_row_rect(index);
        const struct notes_pad *pad = &pads[notes[index].colour];
        uint32_t baseline;
        uint32_t written = 0U;

        if (!notes[index].present || row.width == 0U) {
            continue;
        }
        if (index == selected) {
            status = fill(row, damage, list_selected);
        } else if (index == hover_note) {
            status = fill(row, damage, list_hover);
        }
        if (status != NOTES_STATUS_OK) {
            return status;
        }
        /* A swatch of the note's own colour, so the list says which pad each
         * row is without having to paint the whole row. */
        status = fill((struct ui_rect){ row.x + NOTES_SELECTION_INSET,
            row.y + NOTES_SELECTION_INSET, 6U,
            row.height - NOTES_SELECTION_INSET * 2U }, damage, pad->header);
        if (status == NOTES_STATUS_OK && index == selected) {
            /* And the accent bar Windows marks a selected list row with. */
            status = fill((struct ui_rect){ row.x, row.y + 8U,
                NOTES_SELECTION_BAR, row.height - 16U }, damage,
                pad->accent);
        }
        if (status != NOTES_STATUS_OK) {
            return status;
        }
        baseline = row.y + 26U;
        status = text(damage, row.x + 28U, baseline, notes[index].title, ink);
        if (status != NOTES_STATUS_OK) {
            return status;
        }
        /* Under the title, how many of its items are still open - the thing
         * a list of checklists is actually for. */
        for (size_t line = 0U; line < NOTES_MAX_LINES; ++line) {
            const struct notes_line *item = &notes[index].lines[line];

            if (item->present && item->checkable && !item->done) {
                ++written;
            }
        }
        if (written != 0U) {
            char summary[32];
            size_t at = 0U;

            if (written >= 10U) {
                summary[at++] = (char)('0' + written / 10U);
            }
            summary[at++] = (char)('0' + written % 10U);
            summary[at++] = ' ';
            summary[at++] = 'l';
            summary[at++] = 'e';
            summary[at++] = 'f';
            summary[at++] = 't';
            summary[at] = '\0';
            status = text(damage, row.x + 28U, baseline + 20U, summary,
                ink_soft);
        }
    }
    return status;
}

static enum notes_status draw_pad(struct ui_rect damage)
{
    const struct ui_rect pad = pad_rect();
    const struct notes_pad *paper = &pads[notes[selected].colour];
    enum notes_status status = fill(pad, damage, paper->body);

    if (status == NOTES_STATUS_OK) {
        status = fill((struct ui_rect){ pad.x, pad.y, pad.width,
            NOTES_HEADER_HEIGHT }, damage, paper->header);
    }
    if (status == NOTES_STATUS_OK) {
        status = draw_header_overflow((struct ui_rect){ pad.x, pad.y,
            pad.width, NOTES_HEADER_HEIGHT }, damage, ink);
    }
    if (status == NOTES_STATUS_OK) {
        status = text(damage, pad.x + NOTES_TEXT_INSET,
            pad.y + NOTES_HEADER_HEIGHT + 28U, notes[selected].title, ink);
    }
    if (status != NOTES_STATUS_OK) {
        return status;
    }
    for (size_t index = 0U; index < NOTES_MAX_LINES &&
            status == NOTES_STATUS_OK; ++index) {
        const struct notes_line *line = &notes[selected].lines[index];
        const struct ui_rect row = line_rect(index);
        uint32_t left;

        if (!line->present || row.width == 0U) {
            continue;
        }
        if (index == hover_line) {
            status = fill(row, damage, paper->header);
            if (status != NOTES_STATUS_OK) {
                return status;
            }
        }
        left = row.x;
        if (line->checkable) {
            status = draw_check_box(index, damage, paper);
            if (status != NOTES_STATUS_OK) {
                return status;
            }
            left += NOTES_CHECK_BOX + NOTES_CHECK_GAP;
        }
        uint32_t style = UI_FONT_STYLE_REGULAR;

        if (line->bold) {
            style |= UI_FONT_STYLE_BOLD;
        }
        if (line->italic) {
            style |= UI_FONT_STYLE_ITALIC;
        }
        status = styled_text(damage, left, row.y + 20U, line->text,
            line->done ? ink_done : ink, style);
        if (status == NOTES_STATUS_OK && (line->done || line->strike)) {
            /* Struck through rather than removed, so a finished list still
             * shows what was on it. */
            status = fill((struct ui_rect){ left, row.y + 14U,
                text_width_of(line->text), 1U }, damage, ink_done);
        }
        if (status == NOTES_STATUS_OK && line->underline) {
            status = fill((struct ui_rect){ left, row.y + 22U,
                text_width_of(line->text), 1U }, damage,
                line->done ? ink_done : ink);
        }
        if (status == NOTES_STATUS_OK && index == editing_line && focused) {
            status = fill((struct ui_rect){
                left + text_width_of(line->text) + 1U, row.y + 4U,
                1U, NOTES_LINE_HEIGHT - 8U }, damage, ink);
        }
    }
    if (status == NOTES_STATUS_OK) {
        status = draw_toolbar(damage, paper);
    }
    return status;
}

enum notes_status notes_draw(struct ui_rect damage)
{
    enum notes_status status;

    if (!initialized) {
        return NOTES_STATUS_NOT_INITIALIZED;
    }
    status = fill(window_rect, damage,
        focused ? border_active : border_inactive);
    if (status == NOTES_STATUS_OK) {
        status = draw_list(damage);
    }
    if (status == NOTES_STATUS_OK) {
        status = draw_pad(damage);
    }
    return status;
}

/* ================================================================== INPUT */

enum notes_status notes_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const size_t was_note = hover_note;
    const size_t was_line = hover_line;

    if (damage == NULL) {
        return NOTES_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return NOTES_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    hover_note = (size_t)-1;
    hover_line = (size_t)-1;
    for (size_t index = 0U; index < NOTES_MAX_NOTES; ++index) {
        if (notes[index].present && holds(list_row_rect(index), point)) {
            hover_note = index;
            break;
        }
    }
    for (size_t index = 0U; index < NOTES_MAX_LINES; ++index) {
        if (notes[selected].lines[index].present &&
                holds(line_rect(index), point)) {
            hover_line = index;
            break;
        }
    }
    if (was_note != hover_note || was_line != hover_line) {
        *damage = window_rect;
    }
    return NOTES_STATUS_OK;
}

enum notes_status notes_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return NOTES_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return NOTES_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (holds(new_note_rect(), point)) {
        return notes_new(damage);
    }
    for (size_t tool = 0U; tool < NOTES_TOOL_COUNT; ++tool) {
        if (!holds(toolbar_slot(tool), point)) {
            continue;
        }
        if (editing_line >= NOTES_MAX_LINES ||
                !notes[selected].lines[editing_line].present) {
            for (size_t line = 0U; line < NOTES_MAX_LINES; ++line) {
                if (!notes[selected].lines[line].present) {
                    notes[selected].lines[line].present = true;
                    editing_line = line;
                    break;
                }
            }
        }
        if (editing_line < NOTES_MAX_LINES) {
            struct notes_line *line = &notes[selected].lines[editing_line];

            if (tool == NOTES_TOOL_BOLD) {
                line->bold = !line->bold;
            } else if (tool == NOTES_TOOL_ITALIC) {
                line->italic = !line->italic;
            } else if (tool == NOTES_TOOL_UNDERLINE) {
                line->underline = !line->underline;
            } else if (tool == NOTES_TOOL_STRIKE) {
                line->strike = !line->strike;
            } else {
                line->checkable = !line->checkable;
                if (!line->checkable) {
                    line->done = false;
                }
            }
        }
        *damage = window_rect;
        return NOTES_STATUS_OK;
    }
    for (size_t index = 0U; index < NOTES_MAX_NOTES; ++index) {
        if (notes[index].present && holds(list_row_rect(index), point)) {
            selected = index;
            editing_line = (size_t)-1;
            *damage = window_rect;
            return NOTES_STATUS_OK;
        }
    }
    /*
     * The box toggles checklist state; the rest of the row selects the real
     * insertion line so keyboard input and formatting have an unambiguous
     * destination.
     */
    for (size_t index = 0U; index < NOTES_MAX_LINES; ++index) {
        struct notes_line *line = &notes[selected].lines[index];

        if (line->present && holds(line_rect(index), point)) {
            editing_line = index;
            if (line->checkable && holds(check_rect(index), point)) {
                line->done = !line->done;
            }
            *damage = window_rect;
            return NOTES_STATUS_OK;
        }
    }
    return NOTES_STATUS_OK;
}

/* ============================================================== LIFECYCLE */

static void copy_text(char *destination, const char *source)
{
    size_t index = 0U;

    while (index + 1U < NOTES_TEXT_BYTES && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

enum notes_status notes_new(struct ui_rect *damage)
{
    if (damage == NULL) {
        return NOTES_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return NOTES_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < NOTES_MAX_NOTES; ++index) {
        if (notes[index].present) {
            continue;
        }
        notes[index] = (struct notes_note){
            .present = true,
            .colour = (enum notes_colour)(index % NOTES_COLOUR_COUNT)
        };
        copy_text(notes[index].title, "New note");
        notes[index].lines[0].present = true;
        selected = index;
        editing_line = 0U;
        *damage = window_rect;
        return NOTES_STATUS_OK;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    return NOTES_STATUS_BAD_INDEX;
}

enum notes_status notes_text_input(char character, struct ui_rect *damage)
{
    if (damage == NULL) {
        return NOTES_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return NOTES_STATUS_NOT_INITIALIZED;
    }
    if (character < ' ' || character > '~') {
        *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
        return NOTES_STATUS_OK;
    }
    if (editing_line >= NOTES_MAX_LINES ||
            !notes[selected].lines[editing_line].present) {
        for (size_t index = 0U; index < NOTES_MAX_LINES; ++index) {
            if (!notes[selected].lines[index].present) {
                notes[selected].lines[index].present = true;
                editing_line = index;
                break;
            }
        }
    }
    if (editing_line >= NOTES_MAX_LINES) {
        return NOTES_STATUS_BAD_INDEX;
    }
    struct notes_line *line = &notes[selected].lines[editing_line];
    size_t length = 0U;

    while (length < NOTES_TEXT_BYTES && line->text[length] != '\0') {
        ++length;
    }
    if (length + 1U >= NOTES_TEXT_BYTES) {
        return NOTES_STATUS_BAD_INDEX;
    }
    line->text[length] = character;
    line->text[length + 1U] = '\0';
    *damage = window_rect;
    return NOTES_STATUS_OK;
}

enum notes_status notes_key_backspace(struct ui_rect *damage)
{
    if (damage == NULL) {
        return NOTES_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return NOTES_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (editing_line >= NOTES_MAX_LINES ||
            !notes[selected].lines[editing_line].present) {
        return NOTES_STATUS_OK;
    }
    struct notes_line *line = &notes[selected].lines[editing_line];
    size_t length = 0U;

    while (length < NOTES_TEXT_BYTES && line->text[length] != '\0') {
        ++length;
    }
    if (length != 0U) {
        line->text[length - 1U] = '\0';
        *damage = window_rect;
    }
    return NOTES_STATUS_OK;
}

enum notes_status notes_key_enter(struct ui_rect *damage)
{
    if (damage == NULL) {
        return NOTES_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return NOTES_STATUS_NOT_INITIALIZED;
    }
    const size_t start = editing_line < NOTES_MAX_LINES ?
        editing_line + 1U : 0U;

    for (size_t index = start; index < NOTES_MAX_LINES; ++index) {
        if (!notes[selected].lines[index].present) {
            struct notes_line inherit = { .present = true };

            if (editing_line < NOTES_MAX_LINES) {
                inherit.checkable = notes[selected].lines[editing_line].checkable;
                inherit.bold = notes[selected].lines[editing_line].bold;
                inherit.italic = notes[selected].lines[editing_line].italic;
                inherit.underline = notes[selected].lines[editing_line].underline;
                inherit.strike = notes[selected].lines[editing_line].strike;
            }
            notes[selected].lines[index] = inherit;
            editing_line = index;
            *damage = window_rect;
            return NOTES_STATUS_OK;
        }
    }
    return NOTES_STATUS_BAD_INDEX;
}

enum notes_status notes_get_note(size_t index, struct notes_note *note)
{
    if (note == NULL) {
        return NOTES_STATUS_NULL_ARGUMENT;
    }
    if (index >= NOTES_MAX_NOTES || !notes[index].present) {
        return NOTES_STATUS_BAD_INDEX;
    }
    *note = notes[index];
    return NOTES_STATUS_OK;
}

enum notes_status notes_set_frame(struct ui_rect frame)
{
    const uint32_t least_width = NOTES_BORDER * 2U + NOTES_LIST_WIDTH +
        NOTES_TEXT_INSET * 2U + NOTES_TOOL_SIZE * NOTES_TOOL_COUNT;
    const uint32_t least_height = NOTES_BORDER * 2U + NOTES_HEADER_HEIGHT +
        NOTES_TITLE_HEIGHT + NOTES_LINE_HEIGHT + NOTES_TOOLBAR_HEIGHT;

    if (frame.width < least_width || frame.height < least_height) {
        return NOTES_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return NOTES_STATUS_OK;
}

enum notes_status notes_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum notes_status status;

    if (target == NULL) {
        return NOTES_STATUS_NULL_ARGUMENT;
    }
    status = notes_set_frame(frame);
    if (status != NOTES_STATUS_OK) {
        return status;
    }
    canvas = target;
    selected = 0U;
    editing_line = (size_t)-1;
    initialized = true;
    return NOTES_STATUS_OK;
}

enum notes_status notes_set_note(size_t index, const struct notes_note *note)
{
    if (index >= NOTES_MAX_NOTES) {
        return NOTES_STATUS_BAD_INDEX;
    }
    if (note == NULL) {
        notes[index] = (struct notes_note){ 0 };
        return NOTES_STATUS_OK;
    }
    if ((size_t)note->colour >= NOTES_COLOUR_COUNT) {
        return NOTES_STATUS_BAD_INDEX;
    }
    notes[index] = *note;
    notes[index].present = true;
    notes[index].title[NOTES_TEXT_BYTES - 1U] = '\0';
    return NOTES_STATUS_OK;
}

enum notes_status notes_set_line(size_t note, size_t line,
    const struct notes_line *value)
{
    if (note >= NOTES_MAX_NOTES || line >= NOTES_MAX_LINES) {
        return NOTES_STATUS_BAD_INDEX;
    }
    if (value == NULL) {
        notes[note].lines[line] = (struct notes_line){ 0 };
        return NOTES_STATUS_OK;
    }
    notes[note].lines[line] = *value;
    notes[note].lines[line].present = true;
    notes[note].lines[line].text[NOTES_TEXT_BYTES - 1U] = '\0';
    return NOTES_STATUS_OK;
}

enum notes_status notes_select(size_t index)
{
    if (index >= NOTES_MAX_NOTES || !notes[index].present) {
        return NOTES_STATUS_BAD_INDEX;
    }
    selected = index;
    editing_line = (size_t)-1;
    return NOTES_STATUS_OK;
}

size_t notes_selected(void)
{
    return selected;
}

enum notes_status notes_set_focus(bool active)
{
    focused = active;
    return NOTES_STATUS_OK;
}

/* ============================================================== SELF TEST */

bool notes_self_test(void)
{
    struct notes_note note = { 0 };
    struct notes_line line = { 0 };
    const struct notes_note *saved_first = &notes[0];
    const size_t saved_selected = selected;

    (void)saved_first;
    /* Every pad's header is darker than its body, which is what makes the
     * strip read as a fold rather than as a second panel. */
    for (size_t index = 0U; index < NOTES_COLOUR_COUNT; ++index) {
        const uint32_t body = (uint32_t)pads[index].body.red +
            pads[index].body.green + pads[index].body.blue;
        const uint32_t header = (uint32_t)pads[index].header.red +
            pads[index].header.green + pads[index].header.blue;

        if (header >= body) {
            self_test_failure = "a notes header is not darker than its pad";
            return false;
        }
    }
    /* An index past the end is refused rather than written. */
    copy_text(note.title, "probe");
    if (notes_set_note(NOTES_MAX_NOTES, &note) != NOTES_STATUS_BAD_INDEX) {
        self_test_failure = "notes accepted an index past the end";
        return false;
    }
    if (notes_set_line(0U, NOTES_MAX_LINES, &line) !=
            NOTES_STATUS_BAD_INDEX) {
        self_test_failure = "notes accepted a line past the end";
        return false;
    }
    /* A colour that is not one of the five is refused, because a note that
     * is not one of Sticky Notes' pads is not a Sticky Note. */
    note.colour = (enum notes_colour)NOTES_COLOUR_COUNT;
    if (notes_set_note(0U, &note) != NOTES_STATUS_BAD_INDEX) {
        self_test_failure = "notes accepted a colour off the pad";
        return false;
    }
    selected = saved_selected;
    self_test_failure = "";
    return true;
}

const char *notes_self_test_failure(void)
{
    return self_test_failure;
}
