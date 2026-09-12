/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Settings.  See include/phipia/settings.h for the shape and for what Phipia
 * does differently.
 */

#include <phipia/settings.h>

#include <phipia/cursor.h>

#include <phipia/clock.h>
#include <phipia/framebuffer.h>
#include <phipia/ui_font.h>

#include "heading_font.h"
#include "settings_glyphs.h"
#include "ui_motion.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION: read off a Windows 10 Settings window at 100%
 * scaling.  The four BANDS and the three-column grid are Windows'.
 */

#define SETTINGS_BORDER 1U
#define SETTINGS_CAPTION 32U
#define SETTINGS_CAPTION_BUTTON 46U
/* The account row, which Windows puts above the title rather than in it. */
#define SETTINGS_ACCOUNT 68U
#define SETTINGS_AVATAR 48U
#define SETTINGS_HEADING 62U
#define SETTINGS_SEARCH_HEIGHT 34U
#define SETTINGS_SEARCH_WIDTH 340U
#define SETTINGS_SEARCH_BAND 64U
/* The grid: three tiles across, an icon on the left of each. */
#define SETTINGS_COLUMNS 3U
/*
 * Wide enough for the SUMMARIES, which are the longest strings in the window
 * and the reason it exists: "Uninstall, defaults, optional features" is how
 * anyone finds Apps, and a tile that clips it to "optional featu" has thrown
 * away the half that was doing the work.
 */
#define SETTINGS_TILE_WIDTH 316U
#define SETTINGS_TILE_HEIGHT 76U
#define SETTINGS_TILE_GAP 6U
#define SETTINGS_TILE_ICON 32U   /* a size the glyph set is rasterized at */
#define SETTINGS_TILE_TEXT 52U
#define SETTINGS_PAD 26U
/* Phipia's accent bar on a hovered tile. */
#define SETTINGS_HOVER_BAR 3U

/*
 * A CATEGORY PAGE.
 *
 * Windows 10 puts a navigation column of sub-pages down the left of one of
 * these and the settings themselves on the right.  This has no sub-pages, so
 * it has no column: a heading, and the rows.  The content starts at the same
 * x the home grid starts at, so clicking a tile moves what is on the page
 * without moving where the page begins.
 */
#define SETTINGS_PAGE_HEAD 78U
#define SETTINGS_PAGE_WIDTH 720U
#define SETTINGS_ROW_HEIGHT 58U
#define SETTINGS_GROUP_HEIGHT 46U
#define SETTINGS_PAGE_ICON 32U
/* The controls.  Windows' switch is a rounded track with a knob that slides
 * from one end to the other; these are its measurements. */
#define SETTINGS_SWITCH_WIDTH 44U
#define SETTINGS_SWITCH_HEIGHT 20U
#define SETTINGS_SWITCH_KNOB 10U
#define SETTINGS_SLIDER_WIDTH 190U
#define SETTINGS_SLIDER_TRACK 4U
#define SETTINGS_SLIDER_KNOB 14U
#define SETTINGS_CHOICE_WIDTH 200U
#define SETTINGS_CONTROL_HEIGHT 32U
#define SETTINGS_MENU_ROW 30U
#define SETTINGS_MARK 16U       /* a size the glyph set is rasterized at */
/*
 * The search results, which Windows drops over the grid rather than beside
 * it.  Six is what fits under the box without the panel reaching the first
 * row of tiles, and more than six results means the query is too short to
 * be worth reading anyway.
 */
#define SETTINGS_MAX_RESULTS 6U
#define SETTINGS_RESULT_ROW 46U

/* ================================================================ PALETTE */

struct settings_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define SETTINGS_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct settings_rgb page = SETTINGS_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct settings_rgb rule = SETTINGS_RGB(0xE1U, 0xE1U, 0xE1U);
static const struct settings_rgb ink = SETTINGS_RGB(0x14U, 0x14U, 0x14U);
static const struct settings_rgb ink_soft = SETTINGS_RGB(0x5AU, 0x5AU, 0x5AU);
static const struct settings_rgb ink_faint = SETTINGS_RGB(0x8CU, 0x8CU,
    0x8CU);
static const struct settings_rgb accent = SETTINGS_RGB(0x00U, 0x78U, 0xD7U);
static const struct settings_rgb hover_fill = SETTINGS_RGB(0xF2U, 0xF7U,
    0xFCU);
static const struct settings_rgb field = SETTINGS_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct settings_rgb avatar_fill = SETTINGS_RGB(0xD8U, 0xDEU,
    0xE4U);
static const struct settings_rgb border_active = SETTINGS_RGB(0x00U, 0x78U,
    0xD7U);
static const struct settings_rgb border_inactive = SETTINGS_RGB(0x9BU, 0x9BU,
    0x9BU);

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool focused = true;
static struct settings_tile tiles[SETTINGS_MAX_TILES];
static char account_name[SETTINGS_TEXT_BYTES] = "Phipia";
static char account_detail[SETTINGS_TEXT_BYTES] = "Local account";
static char heading_text[SETTINGS_TEXT_BYTES] = "Phipia Settings";
static size_t hover_tile = (size_t)-1;
/*
 * The tile being LEFT, and the two fades.
 *
 * Windows cross-fades a hover rather than switching it: the row you are
 * leaving is still half lit while the one you have arrived at comes up.  Two
 * motions describe that exactly - one per tile would be fifteen more of them
 * for a state only two can ever be in.
 */
static size_t leaving_tile = (size_t)-1;
static struct ui_motion hover_fade;
static struct ui_motion leave_fade;

/*
 * WHICH SCREEN IS UP, and what is on it.
 *
 * (size_t)-1 is the home grid; anything else is the tile whose page is
 * open.  The rows are indexed by that same tile index rather than by a
 * separate page number, so a category and its settings cannot drift apart -
 * there is no second table to keep in step by hand.
 */
static size_t open_page = (size_t)-1;
static struct settings_row rows[SETTINGS_MAX_TILES][SETTINGS_MAX_ROWS];
static size_t hover_row = (size_t)-1;
static size_t leaving_row = (size_t)-1;
static struct ui_motion row_fade;
static struct ui_motion row_leave_fade;
/* The row a search result opened, drawn with the accent bar until something
 * else is touched - landing on a page at the row you asked for and having to
 * hunt for it is the half of a search result that usually goes missing. */
static size_t lit_row = (size_t)-1;
/* Which CHOICE row has its list open, and which line of it is hovered. */
static size_t open_choice = (size_t)-1;
static size_t choice_hover = (size_t)-1;
/* An ACTION row's press, waiting to be read by whoever can act on it. */
static bool action_waiting;
static size_t action_page;
static size_t action_row;

/*
 * THE SEARCH BOX.  Focus, what is typed into it, and what that matches.
 *
 * A result is a (page, row) pair, where a row of (size_t)-1 means the match
 * was the category itself rather than one of its settings.  They are found
 * by walking the same tables draw_tiles() and draw_rows() draw from, so a
 * category that is not on the grid is not a result either.
 */
static bool search_focused;
static char search_query[SETTINGS_TEXT_BYTES];
static size_t search_query_length;
static size_t result_page[SETTINGS_MAX_RESULTS];
static size_t result_row[SETTINGS_MAX_RESULTS];
static size_t result_count;
static size_t hover_result = (size_t)-1;
static bool caret_visible = true;
static uint32_t caret_phase;
static const char *self_test_failure = "settings self-test has not run";

const char *settings_status_string(enum settings_status status)
{
    switch (status) {
    case SETTINGS_STATUS_OK:
        return "ok";
    case SETTINGS_STATUS_NULL_ARGUMENT:
        return "null argument";
    case SETTINGS_STATUS_NOT_INITIALIZED:
        return "settings not initialized";
    case SETTINGS_STATUS_BAD_INDEX:
        return "settings index is out of range";
    case SETTINGS_STATUS_UNSUPPORTED_GEOMETRY:
        return "settings geometry is unsupported";
    case SETTINGS_STATUS_SURFACE_FAILURE:
        return "settings surface refused a pixel";
    default:
        return "unknown settings status";
    }
}

/* ============================================================== PRIMITIVES */

static uint32_t pack_rgb(struct settings_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return ((uint32_t)colour.red << format.red_position) |
        ((uint32_t)colour.green << format.green_position) |
        ((uint32_t)colour.blue << format.blue_position);
}

static struct settings_rgb unpack_rgb(uint32_t pixel)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return (struct settings_rgb){
        (uint8_t)((pixel >> format.red_position) & 0xFFU),
        (uint8_t)((pixel >> format.green_position) & 0xFFU),
        (uint8_t)((pixel >> format.blue_position) & 0xFFU)
    };
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

static enum settings_status fill(struct ui_rect area, struct ui_rect damage,
    struct settings_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return SETTINGS_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return SETTINGS_STATUS_OK;
}

/* The same, mixed into what is already there - which is what a cross-fade
 * needs and a plain fill cannot do. */
static enum settings_status blend(struct ui_rect area, struct ui_rect damage,
    struct settings_rgb colour, uint32_t alpha)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t over = pack_rgb(colour);

    if (alpha == 0U) {
        return SETTINGS_STATUS_OK;
    }
    if (alpha >= 255U) {
        return fill(area, damage, colour);
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return SETTINGS_STATUS_SURFACE_FAILURE;
            }
            red = (((over >> format.red_position) & 0xFFU) * alpha +
                ((under >> format.red_position) & 0xFFU) * (255U - alpha) +
                127U) / 255U;
            green = (((over >> format.green_position) & 0xFFU) * alpha +
                ((under >> format.green_position) & 0xFFU) * (255U - alpha) +
                127U) / 255U;
            blue = (((over >> format.blue_position) & 0xFFU) * alpha +
                ((under >> format.blue_position) & 0xFFU) * (255U - alpha) +
                127U) / 255U;
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) != SURFACE_STATUS_OK) {
                return SETTINGS_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return SETTINGS_STATUS_OK;
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

/* A filled disc, for the account picture - the one round thing in the app. */
static enum settings_status fill_disc(struct ui_rect box,
    struct ui_rect damage, struct settings_rgb colour)
{
    const int32_t radius = (int32_t)(box.width / 2U);
    const int32_t cx = (int32_t)box.x + radius;
    const int32_t cy = (int32_t)box.y + radius;
    const uint32_t packed = pack_rgb(colour);
    const struct ui_rect clipped = intersect(box, damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const int32_t dx = (int32_t)(clipped.x + x) - cx;
            const int32_t dy = (int32_t)(clipped.y + y) - cy;

            if (dx * dx + dy * dy > radius * radius) {
                continue;
            }
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return SETTINGS_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return SETTINGS_STATUS_OK;
}

static enum settings_status outline(struct ui_rect area,
    struct ui_rect damage, struct settings_rgb colour)
{
    enum settings_status status = fill((struct ui_rect){ area.x, area.y,
        area.width, 1U }, damage, colour);

    if (status == SETTINGS_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y + area.height - 1U,
            area.width, 1U }, damage, colour);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y, 1U, area.height },
            damage, colour);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = fill((struct ui_rect){ area.x + area.width - 1U, area.y, 1U,
            area.height }, damage, colour);
    }
    return status;
}

/* Two string builders, both bounded and both returning where they left the
 * pen so a caller can go straight on appending. */
static size_t append_literal(char *into, size_t capacity, size_t at,
    const char *body)
{
    while (body != NULL && *body != '\0' && at + 1U < capacity) {
        into[at] = *body;
        ++at;
        ++body;
    }
    if (at < capacity) {
        into[at] = '\0';
    }
    return at;
}

static size_t append_uint(char *into, size_t capacity, size_t at,
    uint32_t value)
{
    char digits[12];
    size_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        ++count;
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count > 0U && at + 1U < capacity) {
        --count;
        into[at] = digits[count];
        ++at;
    }
    if (at < capacity) {
        into[at] = '\0';
    }
    return at;
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

static enum settings_status text_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct settings_rgb colour)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (clip.width == 0U || clip.height == 0U || body == NULL ||
            body[0] == '\0') {
        return SETTINGS_STATUS_OK;
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
    return SETTINGS_STATUS_OK;
}

/*
 * The heading, in the larger face.
 *
 * Phipia's font service hands out one size, which is right for a list row
 * and wrong for a title: a window whose heading is body-sized has no
 * hierarchy and reads as a page somebody forgot to finish.  This draws from
 * a second Inter face carried beside the shell - the same typeface as the
 * body text, so the two are one voice at two sizes rather than two voices.
 */
static uint32_t heading_width(const char *body)
{
    uint32_t total = 0U;

    for (const char *scan = body; *scan != '\0'; ++scan) {
        const uint8_t code = (uint8_t)*scan;

        if (code < HEADING_FONT_FIRST || code > HEADING_FONT_LAST) {
            continue;
        }
        total += heading_font[code - HEADING_FONT_FIRST].advance;
    }
    return total;
}

static enum settings_status heading_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct settings_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t over = pack_rgb(colour);
    const struct ui_rect clip = intersect(damage, window_rect);
    uint32_t pen = x;

    if (clip.width == 0U || clip.height == 0U) {
        return SETTINGS_STATUS_OK;
    }
    for (const char *scan = body; *scan != '\0'; ++scan) {
        const uint8_t code = (uint8_t)*scan;

        if (code < HEADING_FONT_FIRST || code > HEADING_FONT_LAST) {
            continue;
        }

        const struct heading_font_glyph *glyph =
            &heading_font[code - HEADING_FONT_FIRST];
        const int32_t left = (int32_t)pen + glyph->bearing;
        const uint32_t top = baseline - HEADING_FONT_BASELINE;

        for (uint32_t row = 0U; row < HEADING_FONT_HEIGHT; ++row) {
            for (uint32_t column = 0U; column < glyph->width; ++column) {
                const int32_t px = left + (int32_t)column;
                const uint32_t py = top + row;
                const uint8_t coverage = heading_font_pixels[glyph->offset +
                    (size_t)row * glyph->width + column];
                uint32_t under;
                uint32_t red;
                uint32_t green;
                uint32_t blue;

                if (coverage == 0U || px < (int32_t)clip.x ||
                        px >= (int32_t)(clip.x + clip.width) ||
                        py < clip.y || py >= clip.y + clip.height) {
                    continue;
                }
                if (surface_read_pixel(canvas, (uint32_t)px, py, &under) !=
                        SURFACE_STATUS_OK) {
                    return SETTINGS_STATUS_SURFACE_FAILURE;
                }
                red = (((over >> format.red_position) & 0xFFU) * coverage +
                    ((under >> format.red_position) & 0xFFU) *
                    (255U - coverage) + 127U) / 255U;
                green = (((over >> format.green_position) & 0xFFU) *
                    coverage + ((under >> format.green_position) & 0xFFU) *
                    (255U - coverage) + 127U) / 255U;
                blue = (((over >> format.blue_position) & 0xFFU) * coverage +
                    ((under >> format.blue_position) & 0xFFU) *
                    (255U - coverage) + 127U) / 255U;
                if (surface_pixel(canvas, (uint32_t)px, py,
                        (red << format.red_position) |
                        (green << format.green_position) |
                        (blue << format.blue_position)) !=
                            SURFACE_STATUS_OK) {
                    return SETTINGS_STATUS_SURFACE_FAILURE;
                }
            }
        }
        pen += glyph->advance;
    }
    return SETTINGS_STATUS_OK;
}

static uint32_t centred_x(uint32_t left, uint32_t width, uint32_t text)
{
    return width > text ? left + (width - text) / 2U : left;
}

static enum settings_status text_clipped(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct settings_rgb colour,
    uint32_t limit)
{
    return text_at(intersect(damage, (struct ui_rect){ x, window_rect.y,
        limit, window_rect.height }), x, baseline, body, colour);
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
    for (size_t index = 0U; index < SETTINGS_LUCIDE_COUNT; ++index) {
        size_t choice = 0U;

        if (name == NULL || !names_match(settings_lucide[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < SETTINGS_LUCIDE_SIZES; ++option) {
            if (settings_lucide_size[option] <= wanted) {
                choice = option;
            }
        }
        *size = settings_lucide_size[choice];
        return settings_lucide[index].alpha[choice];
    }
    return NULL;
}

static enum settings_status draw_glyph(const char *name, struct ui_rect box,
    struct ui_rect damage, struct settings_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t wanted = box.width < box.height ? box.width : box.height;
    const uint32_t over = pack_rgb(colour);
    uint32_t size = 0U;
    const uint8_t *cell = glyph_cell(name, wanted, &size);
    struct ui_rect placed;
    struct ui_rect clipped;

    if (cell == NULL) {
        return SETTINGS_STATUS_OK;
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
                return SETTINGS_STATUS_SURFACE_FAILURE;
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
                    (blue << format.blue_position)) != SURFACE_STATUS_OK) {
                return SETTINGS_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return SETTINGS_STATUS_OK;
}

/* ================================================================ GEOMETRY */

static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){ window_rect.x + SETTINGS_BORDER,
        window_rect.y + SETTINGS_BORDER,
        window_rect.width - SETTINGS_BORDER * 2U, SETTINGS_CAPTION };
}

static struct ui_rect caption_button_rect(uint32_t index)
{
    const struct ui_rect caption = caption_rect();
    const uint32_t from_right = 3U - index;

    return (struct ui_rect){
        caption.x + caption.width - from_right * SETTINGS_CAPTION_BUTTON,
        caption.y, SETTINGS_CAPTION_BUTTON, caption.height };
}

/* The back arrow's own hit target inside the caption. */
static struct ui_rect back_rect(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x + 6U, caption.y, 24U,
        caption.height };
}

static struct ui_rect account_rect(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x, caption.y + caption.height,
        caption.width, SETTINGS_ACCOUNT };
}

/* How wide the grid is, and where it starts: Windows centres it in the
 * window rather than pinning it left, which is why the whole page recentres
 * when you resize it. */
static uint32_t grid_columns(void)
{
    const struct ui_rect caption = caption_rect();
    const uint32_t available = caption.width > SETTINGS_PAD * 2U ?
        caption.width - SETTINGS_PAD * 2U : 0U;

    return available >= SETTINGS_COLUMNS * SETTINGS_TILE_WIDTH +
        (SETTINGS_COLUMNS - 1U) * SETTINGS_TILE_GAP ? SETTINGS_COLUMNS : 2U;
}

static uint32_t grid_width(void)
{
    const uint32_t columns = grid_columns();

    return columns * SETTINGS_TILE_WIDTH +
        (columns - 1U) * SETTINGS_TILE_GAP;
}

static uint32_t grid_left(void)
{
    const struct ui_rect caption = caption_rect();

    return centred_x(caption.x, caption.width, grid_width());
}

static struct ui_rect heading_rect(void)
{
    const struct ui_rect account = account_rect();

    return (struct ui_rect){ account.x, account.y + account.height,
        account.width, SETTINGS_HEADING };
}

static struct ui_rect search_rect(void)
{
    const struct ui_rect head = heading_rect();

    return (struct ui_rect){
        centred_x(head.x, head.width, SETTINGS_SEARCH_WIDTH),
        head.y + head.height + (SETTINGS_SEARCH_BAND -
            SETTINGS_SEARCH_HEIGHT) / 2U,
        SETTINGS_SEARCH_WIDTH, SETTINGS_SEARCH_HEIGHT };
}

static size_t tile_position(size_t index)
{
    size_t position = 0U;

    for (size_t scan = 0U; scan < index; ++scan) {
        if (tiles[scan].present) {
            ++position;
        }
    }
    return position;
}

static struct ui_rect tile_rect(size_t index)
{
    const struct ui_rect head = heading_rect();
    const uint32_t top = head.y + head.height + SETTINGS_SEARCH_BAND;
    const size_t position = tile_position(index);
    const uint32_t columns = grid_columns();
    const uint32_t column = (uint32_t)position % columns;
    const uint32_t row = (uint32_t)position / columns;
    const uint32_t y = top + row * (SETTINGS_TILE_HEIGHT + SETTINGS_TILE_GAP);

    if (!tiles[index].present ||
            y + SETTINGS_TILE_HEIGHT > window_rect.y + window_rect.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){
        grid_left() + column * (SETTINGS_TILE_WIDTH + SETTINGS_TILE_GAP), y,
        SETTINGS_TILE_WIDTH, SETTINGS_TILE_HEIGHT };
}

/*
 * A CATEGORY PAGE's two parts: the heading band under the caption, and the
 * column the rows run down.  The column starts where the home grid starts,
 * so opening a tile changes what the page says without moving where it
 * begins.
 */
static struct ui_rect page_head_rect(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x, caption.y + caption.height,
        caption.width, SETTINGS_PAGE_HEAD };
}

/*
 * The column the rows run down, CENTRED - the same way the home grid is,
 * and for the same reason.  Pinning it to the grid's left edge kept the two
 * screens' left margins in line and left a third of the page empty on the
 * right, which reads as a window that has lost something rather than as a
 * column with air around it.
 */
static struct ui_rect content_rect(void)
{
    const struct ui_rect head = page_head_rect();
    const uint32_t bottom = window_rect.y + window_rect.height -
        SETTINGS_BORDER;
    const uint32_t available = head.width > SETTINGS_PAD * 2U ?
        head.width - SETTINGS_PAD * 2U : head.width;
    const uint32_t width = available < SETTINGS_PAGE_WIDTH ?
        available : SETTINGS_PAGE_WIDTH;

    return (struct ui_rect){
        centred_x(head.x, head.width, width),
        head.y + head.height, width,
        bottom > head.y + head.height ? bottom - head.y - head.height : 0U };
}

static uint32_t row_height(size_t slot, size_t index)
{
    return rows[slot][index].kind == SETTINGS_ROW_HEADING ?
        SETTINGS_GROUP_HEIGHT : SETTINGS_ROW_HEIGHT;
}

/*
 * Where a row sits.  The heights are walked rather than multiplied because
 * a group heading is shorter than a setting - a slot of eight rows with two
 * headings in it is not eight of anything.
 */
static struct ui_rect row_rect(size_t index)
{
    const struct ui_rect content = content_rect();
    uint32_t top = content.y + 6U;

    if (open_page >= SETTINGS_MAX_TILES || index >= SETTINGS_MAX_ROWS ||
            !rows[open_page][index].present) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    for (size_t scan = 0U; scan < index; ++scan) {
        if (rows[open_page][scan].present) {
            top += row_height(open_page, scan);
        }
    }
    if (top + row_height(open_page, index) > content.y + content.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ content.x, top, content.width,
        row_height(open_page, index) };
}

/*
 * The control's own box at the right-hand end of a row - which is also its
 * hit target, because a switch you can flip by clicking the label three
 * hundred pixels away is a switch you flip by accident.  Windows makes the
 * control the target on this slot too.
 */
static struct ui_rect control_rect(size_t index)
{
    const struct ui_rect row = row_rect(index);
    uint32_t width;
    uint32_t height = SETTINGS_CONTROL_HEIGHT;

    if (row.width == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    switch (rows[open_page][index].kind) {
    case SETTINGS_ROW_TOGGLE:
        width = SETTINGS_SWITCH_WIDTH;
        height = SETTINGS_SWITCH_HEIGHT;
        break;
    case SETTINGS_ROW_CHOICE:
        width = SETTINGS_CHOICE_WIDTH;
        break;
    case SETTINGS_ROW_SLIDER:
        width = SETTINGS_SLIDER_WIDTH;
        break;
    case SETTINGS_ROW_ACTION:
        width = width_of(rows[open_page][index].options[0]) + 40U;
        break;
    default:
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ row.x + row.width - width - SETTINGS_PAD,
        row.y + (row.height - height) / 2U, width, height };
}

/* How many options a CHOICE row actually carries - the table is fixed and
 * a row is allowed to use fewer of it than all four. */
static size_t option_count(size_t slot, size_t index)
{
    size_t count = 0U;

    while (count < SETTINGS_MAX_OPTIONS &&
            rows[slot][index].options[count][0] != '\0') {
        ++count;
    }
    return count;
}

static struct ui_rect choice_menu_rect(void)
{
    const struct ui_rect box = control_rect(open_choice);
    const size_t count = open_choice < SETTINGS_MAX_ROWS ?
        option_count(open_page, open_choice) : 0U;

    if (box.width == 0U || count == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ box.x, box.y + box.height + 2U, box.width,
        (uint32_t)count * SETTINGS_MENU_ROW + 8U };
}

static struct ui_rect choice_row_rect(size_t option)
{
    const struct ui_rect menu = choice_menu_rect();

    if (menu.width == 0U ||
            option >= option_count(open_page, open_choice)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ menu.x, menu.y + 4U +
        (uint32_t)option * SETTINGS_MENU_ROW, menu.width,
        SETTINGS_MENU_ROW };
}

/* The results panel, dropped over the grid the way Windows drops its own. */
static struct ui_rect results_rect(void)
{
    const struct ui_rect box = search_rect();

    if (result_count == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ box.x, box.y + box.height + 2U, box.width,
        (uint32_t)result_count * SETTINGS_RESULT_ROW + 8U };
}

static struct ui_rect result_rect(size_t index)
{
    const struct ui_rect panel = results_rect();

    if (panel.width == 0U || index >= result_count) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ panel.x, panel.y + 4U +
        (uint32_t)index * SETTINGS_RESULT_ROW, panel.width,
        SETTINGS_RESULT_ROW };
}

/* The x at the search box's right end that empties it, which exists only
 * while there is something to empty. */
static struct ui_rect search_clear_rect(void)
{
    const struct ui_rect box = search_rect();

    if (search_query[0] == '\0') {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ box.x + box.width - 30U, box.y, 24U,
        box.height };
}

/* ================================================================ MATCHING
 *
 * The same rule the file list's search box uses: case-insensitive, anywhere
 * in the string, and an empty needle matches everything.  Two windows that
 * disagree about what "port" finds would be two windows.
 */

static uint32_t lower_ascii(uint32_t code)
{
    return code >= 'A' && code <= 'Z' ? code + ('a' - 'A') : code;
}

static bool contains_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL) {
        return false;
    }
    if (needle[0] == '\0') {
        return true;
    }
    for (size_t start = 0U; haystack[start] != '\0'; ++start) {
        size_t at = 0U;

        while (needle[at] != '\0' &&
                lower_ascii((uint32_t)(uint8_t)haystack[start + at]) ==
                lower_ascii((uint32_t)(uint8_t)needle[at])) {
            ++at;
        }
        if (needle[at] == '\0') {
            return true;
        }
    }
    return false;
}

/*
 * Rebuild the result list.
 *
 * Categories before their settings, and in the order the grid draws them,
 * so a query that matches both "System" and something inside it puts the
 * slot first - which is where you were going anyway.  Nothing is scored:
 * with a dozen categories and nine rows each there is no ranking problem
 * worth inventing one for, and an order you can predict beats an order that
 * is cleverer.
 */
static void rebuild_results(void)
{
    result_count = 0U;
    hover_result = (size_t)-1;
    if (search_query[0] == '\0') {
        return;
    }
    for (size_t slot = 0U; slot < SETTINGS_MAX_TILES &&
            result_count < SETTINGS_MAX_RESULTS; ++slot) {
        if (!tiles[slot].present) {
            continue;
        }
        if (contains_ci(tiles[slot].name, search_query) ||
                contains_ci(tiles[slot].summary, search_query)) {
            result_page[result_count] = slot;
            result_row[result_count] = (size_t)-1;
            ++result_count;
        }
    }
    for (size_t slot = 0U; slot < SETTINGS_MAX_TILES &&
            result_count < SETTINGS_MAX_RESULTS; ++slot) {
        if (!tiles[slot].present) {
            continue;
        }
        for (size_t index = 0U; index < SETTINGS_MAX_ROWS &&
                result_count < SETTINGS_MAX_RESULTS; ++index) {
            if (!rows[slot][index].present ||
                    rows[slot][index].kind == SETTINGS_ROW_HEADING) {
                continue;
            }
            if (!contains_ci(rows[slot][index].label, search_query) &&
                    !contains_ci(rows[slot][index].detail, search_query)) {
                continue;
            }
            result_page[result_count] = slot;
            result_row[result_count] = index;
            ++result_count;
        }
    }
}

/*
 * A caret in the search box and nowhere else.  Everything else on a
 * Settings page is a toggle, a choice or a slider - controls you press, not
 * text you put a caret in - and Windows leaves the arrow over all of them.
 */
enum cursor_kind settings_cursor_at(struct ui_point point)
{
    if (!initialized || !holds(window_rect, point)) {
        return CURSOR_NORMAL_SELECT;
    }
    if (holds(search_rect(), point)) {
        return CURSOR_TEXT_SELECT;
    }
    return CURSOR_NORMAL_SELECT;
}

struct ui_rect settings_bounds(void)
{
    return window_rect;
}

/* ================================================================== PIECES */

static enum settings_status draw_caption(struct ui_rect damage)
{
    const struct ui_rect caption = caption_rect();
    enum settings_status status = fill(caption, damage, page);

    /* Settings puts a back arrow at the left of its title bar.  It is faint
     * on the home page because there is genuinely nowhere behind it - the
     * one mark in this window allowed to be a picture - and full ink on a
     * category page, where it goes back. */
    if (status == SETTINGS_STATUS_OK) {
        status = draw_glyph("arrow-left", back_rect(), damage,
            open_page == (size_t)-1 ? ink_faint : ink);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = text_at(damage, caption.x + 38U,
            caption.y + caption.height / 2U + 5U, "Settings",
            focused ? ink : ink_faint);
    }
    for (uint32_t index = 0U; index < 3U && status == SETTINGS_STATUS_OK;
         ++index) {
        const struct ui_rect button = caption_button_rect(index);
        const uint32_t size = 10U;
        const uint32_t bx = button.x + (button.width - size) / 2U;
        const uint32_t by = button.y + (button.height - size) / 2U;
        const struct settings_rgb mark = focused ? ink : ink_faint;

        if (index == 0U) {
            status = fill((struct ui_rect){ bx, by + size / 2U, size, 1U },
                damage, mark);
        } else if (index == 1U) {
            status = outline((struct ui_rect){ bx, by, size, size }, damage,
                mark);
        } else {
            for (uint32_t step = 0U; step < size &&
                    status == SETTINGS_STATUS_OK; ++step) {
                status = fill((struct ui_rect){ bx + step, by + step, 1U,
                    1U }, damage, mark);
                if (status == SETTINGS_STATUS_OK) {
                    status = fill((struct ui_rect){ bx + step,
                        by + size - 1U - step, 1U, 1U }, damage, mark);
                }
            }
        }
    }
    return status;
}

static enum settings_status draw_account(struct ui_rect damage)
{
    const struct ui_rect row = account_rect();
    const struct ui_rect avatar = { grid_left(),
        row.y + (row.height - SETTINGS_AVATAR) / 2U,
        SETTINGS_AVATAR, SETTINGS_AVATAR };
    const uint32_t left = avatar.x + SETTINGS_AVATAR + 14U;
    enum settings_status status = fill_disc(avatar, damage, avatar_fill);

    if (status == SETTINGS_STATUS_OK) {
        status = draw_glyph("user", (struct ui_rect){ avatar.x + 10U,
            avatar.y + 10U, SETTINGS_AVATAR - 20U, SETTINGS_AVATAR - 20U },
            damage, ink_soft);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = text_at(damage, left, avatar.y + 22U, account_name, ink);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = text_at(damage, left, avatar.y + 40U, account_detail,
            ink_faint);
    }
    return status;
}

static enum settings_status draw_search(struct ui_rect damage)
{
    const struct ui_rect head = heading_rect();
    const struct ui_rect box = search_rect();
    enum settings_status status = heading_at(damage,
        centred_x(head.x, head.width, heading_width(heading_text)),
        head.y + head.height - 12U, heading_text, ink);

    if (status == SETTINGS_STATUS_OK) {
        status = fill(box, damage, field);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = outline(box, damage, search_focused ? accent : rule);
    }
    if (status == SETTINGS_STATUS_OK) {
        /* Windows underlines the field in the accent, which is the only
         * accent on the whole page until something is hovered.  Focused, it
         * thickens - the one thing on this page that says where the next
         * keystroke is going. */
        status = fill((struct ui_rect){ box.x,
            box.y + box.height - (search_focused ? 3U : 2U), box.width,
            search_focused ? 3U : 2U }, damage, accent);
    }
    /* The magnifier becomes an x once there is a query to clear, which is
     * what the file list's box does and for the same reason: a magnifier
     * you can click to no effect is a button that is not one. */
    if (status == SETTINGS_STATUS_OK && search_query[0] != '\0') {
        status = draw_glyph("x", (struct ui_rect){
            search_clear_rect().x + 4U, box.y,
            SETTINGS_MARK, box.height }, damage, ink_soft);
    } else if (status == SETTINGS_STATUS_OK) {
        status = draw_glyph("search", (struct ui_rect){
            box.x + box.width - 28U, box.y, 20U, box.height }, damage,
            ink_soft);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = text_clipped(damage, box.x + 12U,
            box.y + box.height / 2U + 5U,
            search_query[0] != '\0' ? search_query : "Search settings",
            search_query[0] != '\0' ? ink : ink_faint, box.width - 44U);
    }
    if (status == SETTINGS_STATUS_OK && search_focused && caret_visible) {
        status = fill((struct ui_rect){
            box.x + 12U + width_of(search_query) + 1U, box.y + 7U, 1U,
            box.height - 16U }, damage, ink);
    }
    return status;
}

static enum settings_status draw_tiles(struct ui_rect damage)
{
    enum settings_status status = SETTINGS_STATUS_OK;

    for (size_t index = 0U; index < SETTINGS_MAX_TILES &&
            status == SETTINGS_STATUS_OK; ++index) {
        const struct settings_tile *tile = &tiles[index];
        const struct ui_rect box = tile_rect(index);
        struct settings_rgb mark;

        if (!tile->present || box.width == 0U) {
            continue;
        }
        const uint32_t lift = index == hover_tile ?
            ui_motion_alpha(&hover_fade) : (index == leaving_tile ?
                ui_motion_alpha(&leave_fade) : 0U);

        if (lift > 0U) {
            status = blend(box, damage, hover_fill, lift);
            if (status == SETTINGS_STATUS_OK) {
                status = blend((struct ui_rect){ box.x, box.y,
                    SETTINGS_HOVER_BAR, box.height }, damage, accent, lift);
            }
        }
        if (status != SETTINGS_STATUS_OK) {
            return status;
        }
        /*
         * Its own colour, where the caller gave it one - and the accent
         * where it did not, which is what Windows draws and what this ships
         * with.  See the note in the header for why an earlier version that
         * hued all twelve was reverted.
         */
        mark = tile->colour != 0U ? unpack_rgb(tile->colour) : accent;
        status = draw_glyph(tile->glyph, (struct ui_rect){ box.x + 10U,
            box.y, SETTINGS_TILE_ICON, box.height }, damage, mark);
        if (status != SETTINGS_STATUS_OK) {
            return status;
        }

        const uint32_t left = box.x + SETTINGS_TILE_TEXT;
        const uint32_t limit = box.width - SETTINGS_TILE_TEXT - 10U;

        status = text_clipped(damage, left, box.y + 32U, tile->name, ink,
            limit);
        if (status == SETTINGS_STATUS_OK) {
            /* The summary is not decoration.  "Display, sound,
             * notifications, power" is how anybody actually finds System. */
            status = text_clipped(damage, left, box.y + 50U, tile->summary,
                ink_faint, limit);
        }
    }
    return status;
}

/*
 * A page's heading band: the category's own mark and name, at the size the
 * home page writes "Phipia Settings" in.  Windows writes the category here
 * too, and it is the only thing telling you which of twelve pages you are
 * standing on.
 */
static enum settings_status draw_page_head(struct ui_rect damage)
{
    const struct ui_rect head = page_head_rect();
    const struct settings_tile *tile = &tiles[open_page];
    const struct settings_rgb mark = tile->colour != 0U ?
        unpack_rgb(tile->colour) : accent;
    const uint32_t left = content_rect().x;
    enum settings_status status = fill(head, damage, page);

    /* The mark sits on the HEADING's line rather than centred in the band,
     * because the band is two lines tall and a mark centred across both
     * lands between them. */
    if (status == SETTINGS_STATUS_OK) {
        status = draw_glyph(tile->glyph, (struct ui_rect){ left,
            head.y + 18U, SETTINGS_PAGE_ICON, SETTINGS_PAGE_ICON }, damage,
            mark);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = heading_at(damage, left + SETTINGS_PAGE_ICON + 16U,
            head.y + 43U, tile->name, ink);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = text_at(damage, left + SETTINGS_PAGE_ICON + 18U,
            head.y + 67U, tile->summary, ink_faint);
    }
    return status;
}

/*
 * Windows 10's switch: a rounded track that fills with the accent when it
 * is on, and a knob that sits at whichever end that is.  Off is an outlined
 * track with the knob on the left, which is the state you can read without
 * having learnt the colour.
 */
static enum settings_status draw_switch(struct ui_rect box,
    struct ui_rect damage, bool on)
{
    const uint32_t radius = box.height / 2U;
    const uint32_t knob = SETTINGS_SWITCH_KNOB;
    const uint32_t inset = (box.height - knob) / 2U;
    enum settings_status status;

    if (on) {
        status = fill_disc((struct ui_rect){ box.x, box.y, box.height,
            box.height }, damage, accent);
        if (status == SETTINGS_STATUS_OK) {
            status = fill_disc((struct ui_rect){
                box.x + box.width - box.height, box.y, box.height,
                box.height }, damage, accent);
        }
        if (status == SETTINGS_STATUS_OK) {
            status = fill((struct ui_rect){ box.x + radius, box.y,
                box.width - box.height, box.height }, damage, accent);
        }
        if (status == SETTINGS_STATUS_OK) {
            status = fill_disc((struct ui_rect){
                box.x + box.width - inset - knob, box.y + inset, knob,
                knob }, damage, page);
        }
        return status;
    }
    /* Off: the track drawn as an outline rather than a plate, which is the
     * difference you can see with the colour taken away. */
    status = fill_disc((struct ui_rect){ box.x, box.y, box.height,
        box.height }, damage, ink_soft);
    if (status == SETTINGS_STATUS_OK) {
        status = fill_disc((struct ui_rect){ box.x + box.width - box.height,
            box.y, box.height, box.height }, damage, ink_soft);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = fill((struct ui_rect){ box.x + radius, box.y,
            box.width - box.height, box.height }, damage, ink_soft);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = fill_disc((struct ui_rect){ box.x + 1U, box.y + 1U,
            box.height - 2U, box.height - 2U }, damage, page);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = fill_disc((struct ui_rect){
            box.x + box.width - box.height + 1U, box.y + 1U,
            box.height - 2U, box.height - 2U }, damage, page);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = fill((struct ui_rect){ box.x + radius, box.y + 1U,
            box.width - box.height, box.height - 2U }, damage, page);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = fill_disc((struct ui_rect){ box.x + inset, box.y + inset,
            knob, knob }, damage, ink_soft);
    }
    return status;
}

static enum settings_status draw_slider(struct ui_rect box,
    struct ui_rect damage, uint32_t value)
{
    const uint32_t span = box.width - SETTINGS_SLIDER_KNOB;
    const uint32_t along = span * (value > 100U ? 100U : value) / 100U;
    const uint32_t middle = box.y + box.height / 2U;
    const uint32_t track_y = middle - SETTINGS_SLIDER_TRACK / 2U;
    enum settings_status status = fill((struct ui_rect){
        box.x + SETTINGS_SLIDER_KNOB / 2U, track_y, span,
        SETTINGS_SLIDER_TRACK }, damage, rule);

    if (status == SETTINGS_STATUS_OK && along != 0U) {
        status = fill((struct ui_rect){ box.x + SETTINGS_SLIDER_KNOB / 2U,
            track_y, along, SETTINGS_SLIDER_TRACK }, damage, accent);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = fill_disc((struct ui_rect){ box.x + along,
            middle - SETTINGS_SLIDER_KNOB / 2U, SETTINGS_SLIDER_KNOB,
            SETTINGS_SLIDER_KNOB }, damage, accent);
    }
    return status;
}

static enum settings_status draw_row_control(size_t index,
    struct ui_rect damage)
{
    const struct settings_row *row = &rows[open_page][index];
    const struct ui_rect box = control_rect(index);
    enum settings_status status = SETTINGS_STATUS_OK;

    if (box.width == 0U) {
        return SETTINGS_STATUS_OK;
    }
    switch (row->kind) {
    case SETTINGS_ROW_TOGGLE:
        status = draw_switch(box, damage, row->state != 0U);
        if (status == SETTINGS_STATUS_OK) {
            /* Windows writes On or Off beside its switch, which is the only
             * way to read one in a screenshot or with the colour gone. */
            status = text_at(damage, box.x - 34U,
                box.y + box.height / 2U + 5U,
                row->state != 0U ? "On" : "Off", ink_soft);
        }
        break;
    case SETTINGS_ROW_CHOICE: {
        const size_t count = option_count(open_page, index);
        const size_t chosen = row->state < count ? row->state : 0U;

        status = fill(box, damage, page);
        if (status == SETTINGS_STATUS_OK) {
            status = outline(box, damage,
                index == open_choice ? accent : rule);
        }
        if (status == SETTINGS_STATUS_OK && count != 0U) {
            status = text_clipped(damage, box.x + 10U,
                box.y + box.height / 2U + 5U, row->options[chosen], ink,
                box.width - 34U);
        }
        if (status == SETTINGS_STATUS_OK) {
            status = draw_glyph("chevron-down", (struct ui_rect){
                box.x + box.width - 24U,
                box.y + (box.height - SETTINGS_MARK) / 2U, SETTINGS_MARK,
                SETTINGS_MARK }, damage, ink_soft);
        }
        break;
    }
    case SETTINGS_ROW_SLIDER: {
        char figure[8];
        size_t at = append_uint(figure, sizeof(figure), 0U,
            row->state > 100U ? 100U : row->state);

        (void)append_literal(figure, sizeof(figure), at, "%");
        status = draw_slider(box, damage, row->state);
        if (status == SETTINGS_STATUS_OK) {
            status = text_at(damage, box.x - 42U,
                box.y + box.height / 2U + 5U, figure, ink_soft);
        }
        break;
    }
    case SETTINGS_ROW_ACTION:
        status = fill(box, damage, hover_fill);
        if (status == SETTINGS_STATUS_OK) {
            status = outline(box, damage, rule);
        }
        if (status == SETTINGS_STATUS_OK) {
            status = text_at(damage,
                centred_x(box.x, box.width, width_of(row->options[0])),
                box.y + box.height / 2U + 5U, row->options[0], ink);
        }
        break;
    default:
        break;
    }
    return status;
}

static enum settings_status draw_rows(struct ui_rect damage)
{
    enum settings_status status = SETTINGS_STATUS_OK;

    for (size_t index = 0U; index < SETTINGS_MAX_ROWS &&
            status == SETTINGS_STATUS_OK; ++index) {
        const struct settings_row *row = &rows[open_page][index];
        const struct ui_rect box = row_rect(index);
        uint32_t lift;

        if (!row->present || box.width == 0U) {
            continue;
        }
        if (row->kind == SETTINGS_ROW_HEADING) {
            /* A group title is not a control: no plate, no hover, and the
             * hairline under it is what separates one group from the next. */
            status = text_at(damage, box.x + SETTINGS_PAD,
                box.y + box.height - 14U, row->label, accent);
            if (status == SETTINGS_STATUS_OK) {
                status = fill((struct ui_rect){ box.x + SETTINGS_PAD,
                    box.y + box.height - 7U,
                    box.width - SETTINGS_PAD * 2U, 1U }, damage, rule);
            }
            continue;
        }
        lift = index == hover_row ? ui_motion_alpha(&row_fade) :
            (index == leaving_row ? ui_motion_alpha(&row_leave_fade) : 0U);
        if (index == lit_row) {
            /* Where a search result landed.  It stays lit until the pointer
             * touches something, which is how you find the row you asked
             * for on a page of nine. */
            status = fill(box, damage, hover_fill);
            if (status == SETTINGS_STATUS_OK) {
                status = fill((struct ui_rect){ box.x, box.y,
                    SETTINGS_HOVER_BAR, box.height }, damage, accent);
            }
        } else if (lift > 0U) {
            status = blend(box, damage, hover_fill, lift);
            if (status == SETTINGS_STATUS_OK) {
                status = blend((struct ui_rect){ box.x, box.y,
                    SETTINGS_HOVER_BAR, box.height }, damage, accent, lift);
            }
        }
        if (status != SETTINGS_STATUS_OK) {
            return status;
        }
        {
            const uint32_t left = box.x + SETTINGS_PAD;
            const uint32_t limit = box.width - SETTINGS_PAD -
                (control_rect(index).width + SETTINGS_PAD * 2U);
            const bool two_line = row->detail[0] != '\0';

            status = text_clipped(damage, left,
                two_line ? box.y + 25U : box.y + box.height / 2U + 5U,
                row->label, ink, limit);
            if (status == SETTINGS_STATUS_OK && two_line) {
                status = text_clipped(damage, left, box.y + 43U,
                    row->detail, ink_faint, limit);
            }
        }
        if (status == SETTINGS_STATUS_OK) {
            status = draw_row_control(index, damage);
        }
    }
    return status;
}

/* A CHOICE row's list, floating over the page the way a flyout has to. */
static enum settings_status draw_choice_menu(struct ui_rect damage)
{
    const struct ui_rect menu = choice_menu_rect();
    const size_t count = menu.width == 0U ? 0U :
        option_count(open_page, open_choice);
    enum settings_status status;

    if (menu.width == 0U) {
        return SETTINGS_STATUS_OK;
    }
    status = fill(menu, damage, page);
    if (status == SETTINGS_STATUS_OK) {
        status = outline(menu, damage, rule);
    }
    for (size_t option = 0U; option < count &&
            status == SETTINGS_STATUS_OK; ++option) {
        const struct ui_rect box = choice_row_rect(option);
        const bool chosen = rows[open_page][open_choice].state == option;

        if (option == choice_hover) {
            status = fill(box, damage, hover_fill);
            if (status == SETTINGS_STATUS_OK) {
                status = fill((struct ui_rect){ box.x, box.y,
                    SETTINGS_HOVER_BAR, box.height }, damage, accent);
            }
        }
        if (status == SETTINGS_STATUS_OK && chosen) {
            status = draw_glyph("check", (struct ui_rect){ box.x + 8U,
                box.y + (box.height - SETTINGS_MARK) / 2U, SETTINGS_MARK,
                SETTINGS_MARK }, damage, accent);
        }
        if (status == SETTINGS_STATUS_OK) {
            status = text_clipped(damage, box.x + 30U,
                box.y + box.height / 2U + 5U,
                rows[open_page][open_choice].options[option], ink,
                box.width - 40U);
        }
    }
    return status;
}

/*
 * The results panel.  Each row names the SETTING and, under it, the
 * category it lives in - "Night light" on its own does not tell you where
 * clicking it is about to take you.
 */
static enum settings_status draw_results(struct ui_rect damage)
{
    const struct ui_rect panel = results_rect();
    enum settings_status status;

    if (panel.width == 0U) {
        return SETTINGS_STATUS_OK;
    }
    status = fill(panel, damage, page);
    if (status == SETTINGS_STATUS_OK) {
        status = outline(panel, damage, rule);
    }
    for (size_t index = 0U; index < result_count &&
            status == SETTINGS_STATUS_OK; ++index) {
        const struct ui_rect box = result_rect(index);
        const size_t slot = result_page[index];
        const size_t which = result_row[index];
        const char *label = which == (size_t)-1 ? tiles[slot].name :
            rows[slot][which].label;

        if (index == hover_result) {
            status = fill(box, damage, hover_fill);
            if (status == SETTINGS_STATUS_OK) {
                status = fill((struct ui_rect){ box.x, box.y,
                    SETTINGS_HOVER_BAR, box.height }, damage, accent);
            }
        }
        if (status == SETTINGS_STATUS_OK) {
            status = draw_glyph(tiles[slot].glyph, (struct ui_rect){
                box.x + 12U, box.y + (box.height - SETTINGS_MARK) / 2U,
                SETTINGS_MARK, SETTINGS_MARK }, damage,
                tiles[slot].colour != 0U ? unpack_rgb(tiles[slot].colour) :
                    accent);
        }
        if (status == SETTINGS_STATUS_OK) {
            status = text_clipped(damage, box.x + 40U, box.y + 21U, label,
                ink, box.width - 50U);
        }
        if (status == SETTINGS_STATUS_OK) {
            /* Where it lives.  A category result says so too rather than
             * repeating its own name, which would read as a stutter. */
            status = text_clipped(damage, box.x + 40U, box.y + 38U,
                which == (size_t)-1 ? tiles[slot].summary :
                    tiles[slot].name, ink_faint, box.width - 50U);
        }
    }
    return status;
}

enum settings_status settings_draw(struct ui_rect damage)
{
    enum settings_status status;

    if (!initialized) {
        return SETTINGS_STATUS_NOT_INITIALIZED;
    }
    status = fill(window_rect, damage,
        focused ? border_active : border_inactive);
    if (status == SETTINGS_STATUS_OK) {
        status = fill((struct ui_rect){ window_rect.x + SETTINGS_BORDER,
            window_rect.y + SETTINGS_BORDER,
            window_rect.width - SETTINGS_BORDER * 2U,
            window_rect.height - SETTINGS_BORDER * 2U }, damage, page);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = draw_caption(damage);
    }
    if (open_page != (size_t)-1) {
        /* A category page: no account row and no search box, which is where
         * Windows puts the page's own heading instead. */
        if (status == SETTINGS_STATUS_OK) {
            status = draw_page_head(damage);
        }
        if (status == SETTINGS_STATUS_OK) {
            status = draw_rows(damage);
        }
        if (status == SETTINGS_STATUS_OK) {
            status = draw_choice_menu(damage);
        }
        return status;
    }
    if (status == SETTINGS_STATUS_OK) {
        status = draw_account(damage);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = draw_search(damage);
    }
    if (status == SETTINGS_STATUS_OK) {
        status = draw_tiles(damage);
    }
    if (status == SETTINGS_STATUS_OK) {
        /* Over the grid, the way Windows drops its own. */
        status = draw_results(damage);
    }
    return status;
}

/* ================================================================== INPUT */

/*
 * NAVIGATION.
 *
 * Everything that changes which screen is up goes through these two, so a
 * tile, a search result and the back arrow cannot each leave a different
 * half of the state behind.
 */
static void clear_transient(void)
{
    hover_tile = (size_t)-1;
    leaving_tile = (size_t)-1;
    hover_row = (size_t)-1;
    leaving_row = (size_t)-1;
    hover_result = (size_t)-1;
    open_choice = (size_t)-1;
    choice_hover = (size_t)-1;
    ui_motion_reset(&hover_fade, 0);
    ui_motion_reset(&leave_fade, 0);
    ui_motion_reset(&row_fade, 0);
    ui_motion_reset(&row_leave_fade, 0);
}

size_t settings_open_page(void)
{
    return open_page;
}

enum settings_status settings_go_to_page(size_t slot, struct ui_rect *damage)
{
    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return SETTINGS_STATUS_NOT_INITIALIZED;
    }
    if (slot >= SETTINGS_MAX_TILES || !tiles[slot].present) {
        return SETTINGS_STATUS_BAD_INDEX;
    }
    clear_transient();
    open_page = slot;
    lit_row = (size_t)-1;
    /* Navigating spends the query.  Leaving it set would put the results
     * panel back over the grid on the way home, offering to take you
     * somewhere you have just been. */
    search_focused = false;
    search_query[0] = '\0';
    search_query_length = 0U;
    result_count = 0U;
    *damage = window_rect;
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_go_back(struct ui_rect *damage)
{
    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized || open_page == (size_t)-1) {
        return SETTINGS_STATUS_OK;
    }
    clear_transient();
    open_page = (size_t)-1;
    lit_row = (size_t)-1;
    *damage = window_rect;
    return SETTINGS_STATUS_OK;
}

bool settings_take_action(size_t *slot, size_t *index)
{
    if (!action_waiting) {
        return false;
    }
    if (slot != NULL) {
        *slot = action_page;
    }
    if (index != NULL) {
        *index = action_row;
    }
    action_waiting = false;
    return true;
}

uint32_t settings_row_state(size_t slot, size_t index)
{
    if (slot >= SETTINGS_MAX_TILES || index >= SETTINGS_MAX_ROWS ||
            !rows[slot][index].present) {
        return 0U;
    }
    return rows[slot][index].state;
}

/* ================================================================= SEARCH */

static void set_search_focus(bool wanted)
{
    if (wanted) {
        open_choice = (size_t)-1;
        caret_visible = true;
        caret_phase = (uint32_t)(clock_monotonic_ns() /
            UINT64_C(530000000)) % 2U;
    }
    search_focused = wanted;
}

/* Opens the page a result names, with its row lit.  A category result has
 * no row, so it just opens the page. */
static enum settings_status run_result(size_t index, struct ui_rect *damage)
{
    size_t which;

    if (index >= result_count) {
        return SETTINGS_STATUS_OK;
    }
    which = result_row[index];
    (void)settings_go_to_page(result_page[index], damage);
    lit_row = which;
    search_query[0] = '\0';
    search_query_length = 0U;
    result_count = 0U;
    hover_result = (size_t)-1;
    *damage = window_rect;
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_focus_search(bool wanted,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return SETTINGS_STATUS_NOT_INITIALIZED;
    }
    /* There is no box to focus on a category page - see the header. */
    if (wanted && open_page != (size_t)-1) {
        return SETTINGS_STATUS_OK;
    }
    set_search_focus(wanted);
    *damage = window_rect;
    return SETTINGS_STATUS_OK;
}

bool settings_search_focused(void)
{
    return search_focused;
}

const char *settings_search_query(void)
{
    return search_query;
}

size_t settings_search_result_count(void)
{
    return result_count;
}

enum settings_status settings_text_input(char character,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized || !search_focused) {
        return SETTINGS_STATUS_OK;
    }
    /* Anything printable; a search box is filtering text, not naming
     * anything, so there is nothing here to reserve. */
    if (character < 0x20 || (unsigned char)character > 0x7EU) {
        return SETTINGS_STATUS_OK;
    }
    if (search_query_length + 1U >= sizeof(search_query)) {
        return SETTINGS_STATUS_OK;
    }
    search_query[search_query_length] = character;
    ++search_query_length;
    search_query[search_query_length] = '\0';
    rebuild_results();
    caret_visible = true;
    *damage = window_rect;
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_key_backspace(struct ui_rect *damage)
{
    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized || !search_focused || search_query_length == 0U) {
        return SETTINGS_STATUS_OK;
    }
    --search_query_length;
    search_query[search_query_length] = '\0';
    rebuild_results();
    caret_visible = true;
    *damage = window_rect;
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_key_enter(struct ui_rect *damage)
{
    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized || !search_focused || result_count == 0U) {
        return SETTINGS_STATUS_OK;
    }
    return run_result(0U, damage);
}

enum settings_status settings_key_escape(struct ui_rect *damage)
{
    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return SETTINGS_STATUS_NOT_INITIALIZED;
    }
    if (open_choice != (size_t)-1) {
        open_choice = (size_t)-1;
        choice_hover = (size_t)-1;
        *damage = window_rect;
        return SETTINGS_STATUS_OK;
    }
    if (open_page != (size_t)-1) {
        return settings_go_back(damage);
    }
    if (search_focused || search_query[0] != '\0') {
        search_query[0] = '\0';
        search_query_length = 0U;
        rebuild_results();
        set_search_focus(false);
        *damage = window_rect;
    }
    return SETTINGS_STATUS_OK;
}

/* ================================================================== INPUT */

enum settings_status settings_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const size_t was_tile = hover_tile;
    const size_t was_row = hover_row;
    const size_t was_result = hover_result;
    const uint64_t now = clock_monotonic_ns();

    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return SETTINGS_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    hover_tile = (size_t)-1;
    hover_row = (size_t)-1;
    hover_result = (size_t)-1;
    if (open_choice != (size_t)-1) {
        /* The list owns the pointer while it is open. */
        const size_t was_choice = choice_hover;
        const size_t count = option_count(open_page, open_choice);

        choice_hover = (size_t)-1;
        for (size_t option = 0U; option < count; ++option) {
            if (holds(choice_row_rect(option), point)) {
                choice_hover = option;
                break;
            }
        }
        if (was_choice != choice_hover) {
            *damage = choice_menu_rect();
        }
        return SETTINGS_STATUS_OK;
    }
    if (open_page != (size_t)-1) {
        for (size_t index = 0U; index < SETTINGS_MAX_ROWS; ++index) {
            if (rows[open_page][index].kind == SETTINGS_ROW_HEADING) {
                continue;
            }
            if (holds(row_rect(index), point)) {
                hover_row = index;
                break;
            }
        }
    } else {
        for (size_t index = 0U; index < result_count; ++index) {
            if (holds(result_rect(index), point)) {
                hover_result = index;
                break;
            }
        }
        /* A tile under the results panel is not hovered - the panel is on
         * top of it, and hot-tracking through a flyout is how you end up
         * clicking what you cannot see. */
        if (hover_result == (size_t)-1 && !holds(results_rect(), point)) {
            for (size_t index = 0U; index < SETTINGS_MAX_TILES; ++index) {
                if (tiles[index].present && holds(tile_rect(index), point)) {
                    hover_tile = index;
                    break;
                }
            }
        }
    }
    if (was_tile != hover_tile) {
        leaving_tile = was_tile;
        ui_motion_reset(&leave_fade, hover_fade.value);
        ui_motion_to(&leave_fade, 0, UI_MOTION_BRUSH_NS, now);
        ui_motion_reset(&hover_fade, 0);
        if (hover_tile != (size_t)-1) {
            ui_motion_to(&hover_fade, (int32_t)UI_MOTION_ONE,
                UI_MOTION_BRUSH_NS, now);
        }
        *damage = window_rect;
    }
    if (was_row != hover_row) {
        leaving_row = was_row;
        ui_motion_reset(&row_leave_fade, row_fade.value);
        ui_motion_to(&row_leave_fade, 0, UI_MOTION_BRUSH_NS, now);
        ui_motion_reset(&row_fade, 0);
        if (hover_row != (size_t)-1) {
            ui_motion_to(&row_fade, (int32_t)UI_MOTION_ONE,
                UI_MOTION_BRUSH_NS, now);
        }
        /* Touching a row puts out the light a search result left on it -
         * you have found the row, so the marker has done its job. */
        lit_row = (size_t)-1;
        *damage = window_rect;
    }
    if (was_result != hover_result) {
        *damage = join(*damage, results_rect());
    }
    return SETTINGS_STATUS_OK;
}

static struct ui_rect hover_damage(size_t index)
{
    if (index >= SETTINGS_MAX_TILES) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return tile_rect(index);
}

static struct ui_rect row_damage(size_t index)
{
    if (index >= SETTINGS_MAX_ROWS) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return row_rect(index);
}

bool settings_animate(struct ui_rect *damage)
{
    const uint64_t now = clock_monotonic_ns();
    bool moved = false;

    if (damage == NULL || !initialized) {
        return false;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (ui_motion_advance(&hover_fade, now, ui_ease_linear)) {
        *damage = join(*damage, hover_damage(hover_tile));
        moved = true;
    }
    if (ui_motion_advance(&leave_fade, now, ui_ease_linear)) {
        *damage = join(*damage, hover_damage(leaving_tile));
        moved = true;
    }
    if (ui_motion_advance(&row_fade, now, ui_ease_linear)) {
        *damage = join(*damage, row_damage(hover_row));
        moved = true;
    }
    if (ui_motion_advance(&row_leave_fade, now, ui_ease_linear)) {
        *damage = join(*damage, row_damage(leaving_row));
        moved = true;
    }
    if (search_focused) {
        /* One caret, in half-second halves - fully on or fully off, so
         * there is nothing here for ui_motion.h's interpolation to do and
         * only the phase CROSSING a boundary is a redraw. */
        const uint32_t phase = (uint32_t)(now / UINT64_C(530000000)) % 2U;

        if (phase != caret_phase) {
            caret_phase = phase;
            caret_visible = !caret_visible;
            *damage = join(*damage, search_rect());
            moved = true;
        }
    }
    return moved;
}

bool settings_animating(void)
{
    return ui_motion_running(&hover_fade) || ui_motion_running(&leave_fade) ||
        ui_motion_running(&row_fade) || ui_motion_running(&row_leave_fade) ||
        search_focused;
}

/* One press of one control.  Every kind does the thing it is drawn as; see
 * the note on enum settings_row_kind for what that does and does not
 * reach. */
static enum settings_status press_row(size_t index, struct ui_point point,
    struct ui_rect *damage)
{
    struct settings_row *row = &rows[open_page][index];
    const struct ui_rect box = control_rect(index);

    lit_row = (size_t)-1;
    *damage = window_rect;
    switch (row->kind) {
    case SETTINGS_ROW_TOGGLE:
        row->state = row->state != 0U ? 0U : 1U;
        break;
    case SETTINGS_ROW_CHOICE:
        open_choice = index;
        choice_hover = (size_t)-1;
        break;
    case SETTINGS_ROW_SLIDER: {
        /* Set to where you pressed.  This platform delivers a press and a
         * move but no drag, so a slider that could only be dragged would be
         * a slider that could not be used; Windows sets one on a click of
         * the track too. */
        const uint32_t span = box.width - SETTINGS_SLIDER_KNOB;
        const uint32_t left = box.x + SETTINGS_SLIDER_KNOB / 2U;
        uint32_t along;

        if (point.x <= (int32_t)left) {
            along = 0U;
        } else if ((uint32_t)point.x >= left + span) {
            along = span;
        } else {
            along = (uint32_t)point.x - left;
        }
        row->state = span == 0U ? 0U : along * 100U / span;
        break;
    }
    case SETTINGS_ROW_ACTION:
        action_waiting = true;
        action_page = open_page;
        action_row = index;
        break;
    default:
        break;
    }
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return SETTINGS_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (open_choice != (size_t)-1) {
        /* A hit takes that option; a miss anywhere - including back on the
         * control that opened it - dismisses the list, which is the one
         * click a flyout is allowed to spend on itself. */
        const size_t count = option_count(open_page, open_choice);

        for (size_t option = 0U; option < count; ++option) {
            if (holds(choice_row_rect(option), point)) {
                rows[open_page][open_choice].state = (uint32_t)option;
                break;
            }
        }
        open_choice = (size_t)-1;
        choice_hover = (size_t)-1;
        *damage = window_rect;
        return SETTINGS_STATUS_OK;
    }
    if (holds(back_rect(), point)) {
        return settings_go_back(damage);
    }
    if (open_page != (size_t)-1) {
        for (size_t index = 0U; index < SETTINGS_MAX_ROWS; ++index) {
            const enum settings_row_kind kind = rows[open_page][index].kind;
            const struct ui_rect control = control_rect(index);

            if (control.width == 0U) {
                continue;
            }
            /*
             * HOW BIG THE TARGET IS, which is two answers rather than one.
             *
             * A switch and a choice take the WHOLE ROW: the label is what
             * you are aiming at, the row is what lights up under the
             * pointer, and a row that highlights and then ignores the click
             * is the worst of both.  Windows takes only the control here;
             * this is the one place the copy is deliberately more forgiving
             * than the original.
             *
             * A slider and a button take only their own control, and for
             * reasons rather than symmetry: a slider is set to WHERE it is
             * pressed, so a click on the label would slam it to zero, and a
             * button is a button - "Reset this PC" firing because you
             * clicked the sentence next to it would be the worst press in
             * the window.
             */
            const bool whole_row = kind == SETTINGS_ROW_TOGGLE ||
                kind == SETTINGS_ROW_CHOICE;

            if (holds(whole_row ? row_rect(index) : control, point)) {
                return press_row(index, point, damage);
            }
        }
        return SETTINGS_STATUS_OK;
    }
    /* Home. */
    if (holds(search_clear_rect(), point)) {
        search_query[0] = '\0';
        search_query_length = 0U;
        rebuild_results();
        set_search_focus(true);
        *damage = window_rect;
        return SETTINGS_STATUS_OK;
    }
    if (holds(search_rect(), point)) {
        set_search_focus(true);
        *damage = window_rect;
        return SETTINGS_STATUS_OK;
    }
    for (size_t index = 0U; index < result_count; ++index) {
        if (holds(result_rect(index), point)) {
            return run_result(index, damage);
        }
    }
    if (holds(results_rect(), point)) {
        return SETTINGS_STATUS_OK;   /* the panel eats its own gaps */
    }
    if (search_focused) {
        set_search_focus(false);
        *damage = window_rect;
    }
    for (size_t index = 0U; index < SETTINGS_MAX_TILES; ++index) {
        if (tiles[index].present && holds(tile_rect(index), point)) {
            return settings_go_to_page(index, damage);
        }
    }
    return SETTINGS_STATUS_OK;
}

/* ============================================================== LIFECYCLE */

static void copy_field(char *destination, const char *source)
{
    size_t index = 0U;

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    while (index + 1U < SETTINGS_TEXT_BYTES && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

enum settings_status settings_set_frame(struct ui_rect frame)
{
    const uint32_t least_width = SETTINGS_BORDER * 2U + 640U;
    const uint32_t least_height = SETTINGS_BORDER * 2U + SETTINGS_CAPTION +
        SETTINGS_ACCOUNT + SETTINGS_HEADING + SETTINGS_SEARCH_BAND +
        SETTINGS_TILE_HEIGHT;

    /* Compact framebuffers use a two-column home grid and a correspondingly
     * narrower category page. */
    if (frame.width < least_width || frame.height < least_height) {
        return SETTINGS_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum settings_status status;

    if (target == NULL) {
        return SETTINGS_STATUS_NULL_ARGUMENT;
    }
    canvas = target;
    status = settings_set_frame(frame);
    if (status != SETTINGS_STATUS_OK) {
        return status;
    }
    open_page = (size_t)-1;
    lit_row = (size_t)-1;
    open_choice = (size_t)-1;
    choice_hover = (size_t)-1;
    hover_tile = (size_t)-1;
    hover_row = (size_t)-1;
    hover_result = (size_t)-1;
    leaving_tile = (size_t)-1;
    leaving_row = (size_t)-1;
    action_waiting = false;
    search_focused = false;
    search_query[0] = '\0';
    search_query_length = 0U;
    result_count = 0U;
    caret_visible = true;
    caret_phase = 0U;
    initialized = true;
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_set_tile(size_t index,
    const struct settings_tile *tile)
{
    if (index >= SETTINGS_MAX_TILES) {
        return SETTINGS_STATUS_BAD_INDEX;
    }
    if (tile == NULL) {
        tiles[index] = (struct settings_tile){ 0 };
        return SETTINGS_STATUS_OK;
    }
    tiles[index] = *tile;
    tiles[index].present = true;
    tiles[index].name[SETTINGS_TEXT_BYTES - 1U] = '\0';
    tiles[index].summary[SETTINGS_TEXT_BYTES - 1U] = '\0';
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_set_row(size_t slot, size_t index,
    const struct settings_row *row)
{
    if (slot >= SETTINGS_MAX_TILES || index >= SETTINGS_MAX_ROWS) {
        return SETTINGS_STATUS_BAD_INDEX;
    }
    if (row == NULL) {
        rows[slot][index] = (struct settings_row){ 0 };
        return SETTINGS_STATUS_OK;
    }
    if ((size_t)row->kind >= SETTINGS_ROW_KIND_COUNT) {
        return SETTINGS_STATUS_BAD_INDEX;
    }
    /* A CHOICE with no options is a control with nothing behind it - the
     * exact thing this window is being cleared of - so it is refused
     * rather than drawn empty. */
    if ((row->kind == SETTINGS_ROW_CHOICE ||
            row->kind == SETTINGS_ROW_ACTION) &&
            row->options[0][0] == '\0') {
        return SETTINGS_STATUS_BAD_INDEX;
    }
    rows[slot][index] = *row;
    rows[slot][index].present = true;
    rows[slot][index].label[SETTINGS_TEXT_BYTES - 1U] = '\0';
    rows[slot][index].detail[SETTINGS_TEXT_BYTES - 1U] = '\0';
    for (size_t option = 0U; option < SETTINGS_MAX_OPTIONS; ++option) {
        rows[slot][index].options[option][SETTINGS_OPTION_BYTES - 1U] = '\0';
    }
    /* A slider is a percentage and a choice indexes its own list; neither
     * is allowed to arrive pointing past its own range. */
    if (row->kind == SETTINGS_ROW_SLIDER && rows[slot][index].state > 100U) {
        rows[slot][index].state = 100U;
    }
    if (row->kind == SETTINGS_ROW_CHOICE &&
            rows[slot][index].state >= option_count(slot, index)) {
        rows[slot][index].state = 0U;
    }
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_set_account(const char *name,
    const char *detail)
{
    copy_field(account_name, name);
    copy_field(account_detail, detail);
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_set_heading(const char *heading)
{
    copy_field(heading_text, heading);
    return SETTINGS_STATUS_OK;
}

enum settings_status settings_set_focus(bool active)
{
    focused = active;
    return SETTINGS_STATUS_OK;
}

/* ============================================================== SELF TEST */

bool settings_self_test(void)
{
    struct settings_tile probe = { 0 };
    uint32_t size = 0U;

    if (settings_set_tile(SETTINGS_MAX_TILES, &probe) !=
            SETTINGS_STATUS_BAD_INDEX) {
        self_test_failure = "settings accepted an index past the end";
        return false;
    }
    /* Three to a row, and the grid has to fit the narrowest window this will
     * take - or the third column is silently cut off. */
    if (grid_width() + SETTINGS_PAD * 2U > 1024U) {
        self_test_failure = "the settings grid is wider than a screen";
        return false;
    }
    /*
     * Every tile that has been given a glyph has to HAVE that glyph.  A
     * missing one draws nothing at all, which on a grid of thirteen reads as
     * a tile that is merely indented rather than as an error.
     */
    for (size_t index = 0U; index < SETTINGS_MAX_TILES; ++index) {
        if (!tiles[index].present || tiles[index].glyph == NULL) {
            continue;
        }
        if (glyph_cell(tiles[index].glyph, SETTINGS_TILE_ICON, &size) ==
                NULL) {
            self_test_failure = "a settings category has no icon";
            return false;
        }
    }
    /*
     * Every mark this window draws, at the size it draws it.  The grid's
     * icons are checked above from the caller's own table; these are the
     * ones the module names itself, and a missing one is a control that
     * silently loses its chevron or its tick.
     */
    {
        static const char *const marks[] = { "arrow-left", "search", "x",
            "chevron-down", "check", "user" };

        for (size_t index = 0U;
             index < sizeof(marks) / sizeof(marks[0]); ++index) {
            if (glyph_cell(marks[index], SETTINGS_MARK, &size) == NULL) {
                self_test_failure = "a settings mark is missing";
                return false;
            }
        }
    }
    /*
     * NOTHING IS RESAMPLED.  Every box this window draws a mark into has to
     * be a size the glyph set is rasterized at, because the drawing picks
     * the largest cell that FITS and composites it one to one: a box that
     * is not native gets a smaller mark floating in it, and a box smaller
     * than the smallest cell gets one that overflows into whatever is
     * beside it.
     */
    {
        static const uint32_t boxes[] = { SETTINGS_TILE_ICON,
            SETTINGS_PAGE_ICON, SETTINGS_MARK };

        for (size_t index = 0U;
             index < sizeof(boxes) / sizeof(boxes[0]); ++index) {
            bool found = false;

            for (size_t option = 0U; option < SETTINGS_LUCIDE_SIZES;
                 ++option) {
                found = found || settings_lucide_size[option] ==
                    boxes[index];
            }
            if (!found) {
                self_test_failure = "a mark box is not a native glyph size";
                return false;
            }
        }
    }
    /* A page has to fit in the window it opens in, or a row's control runs
     * off the right-hand edge. */
    if (content_rect().width + SETTINGS_PAD * 2U > caption_rect().width) {
        self_test_failure = "a settings page is wider than its window";
        return false;
    }
    /* The matching rule, which every result on the page runs through. */
    if (!contains_ci("Bluetooth", "blue") ||
            !contains_ci("Bluetooth", "TOOTH") ||
            !contains_ci("Bluetooth", "")) {
        self_test_failure = "the search box refused a name it matches";
        return false;
    }
    if (contains_ci("Bluetooth", "z") ||
            contains_ci("Bluetooth", "bluetooths")) {
        self_test_failure = "the search box matched a name it should not";
        return false;
    }
    self_test_failure = "";
    return true;
}

const char *settings_self_test_failure(void)
{
    return self_test_failure;
}
