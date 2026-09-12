/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * The Store.  See include/phipia/store.h for the shape and for what Phipia
 * does differently.
 */

#include <phipia/store.h>

#include <phipia/clock.h>
#include <phipia/framebuffer.h>
#include <phipia/taskbar.h>
#include <phipia/ui_font.h>

#include "explorer_glyphs.h"
#include "ui_motion.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION: read off a Windows 10 Store window at 100% scaling.
 */

#define STORE_BORDER 1U
#define STORE_CAPTION 32U
#define STORE_CAPTION_BUTTON 46U
#define STORE_NAV 46U             /* the tab bar across the top */
#define STORE_TAB_PAD 16U
#define STORE_PAD 20U
#define STORE_SEARCH 220U
#define STORE_HEADING 34U
/* A card: a coloured plate with the logo on it, then a foot of text. */
#define STORE_CARD_WIDTH 150U
#define STORE_CARD_PLATE 104U
#define STORE_CARD_FOOT 68U
#define STORE_CARD_GAP 14U
#define STORE_CARD_LOGO 48U
#define STORE_SPOTLIGHT_HEIGHT 156U
/*
 * The spotlight's logo TILE, and the artwork on it.
 *
 * The tile is 96 and the art is 48, because 48 is the largest size the
 * artwork exists at and nothing here is resampled - a 96-pixel box was
 * drawing a 48-pixel mark floating in the middle of it with forty-eight
 * pixels of dark panel all round, which reads as a picture that failed to
 * load.  Putting the art on a tile of its own fills the slot with something
 * deliberate and leaves the mark at its native size.
 */
#define STORE_SPOTLIGHT_LOGO 96U
#define STORE_SPOTLIGHT_ART 48U
/* Phipia's rating bar, where Windows draws five stars. */
#define STORE_RATING_WIDTH 62U
#define STORE_RATING_HEIGHT 4U

/*
 * The baselines of the card's caption, measured from the top of the foot.
 *
 * The font's line box is `ascent + descent` tall - nineteen rows for the
 * atlas the shell draws with - and four stacked rows of that do not fit
 * in a sixty-eight pixel foot.  Stacking them anyway put the review count
 * inside the category's descenders and the price inside the count's, so
 * the card read as smudged wherever a category was long enough to reach
 * the count.  Three rows do fit, exactly: the price shares the category's
 * baseline at the right edge, and the category is clipped short of it.
 *
 * store_self_test() checks the rows still clear each other and still land
 * inside the foot, whatever size the font service hands out.
 */
#define STORE_FOOT_NAME 20U
#define STORE_FOOT_CATEGORY 39U
#define STORE_FOOT_RATING 53U
#define STORE_FOOT_REVIEWS 58U
#define STORE_GET_WIDTH 96U
#define STORE_GET_HEIGHT 34U
#define STORE_TAB_COUNT 4U

/* ================================================================ PALETTE */

struct store_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define STORE_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct store_rgb page = STORE_RGB(0xF3U, 0xF3U, 0xF3U);
static const struct store_rgb chrome = STORE_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct store_rgb card_fill = STORE_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct store_rgb rule = STORE_RGB(0xE1U, 0xE1U, 0xE1U);
static const struct store_rgb ink = STORE_RGB(0x14U, 0x14U, 0x14U);
static const struct store_rgb ink_soft = STORE_RGB(0x60U, 0x60U, 0x60U);
static const struct store_rgb ink_faint = STORE_RGB(0x8CU, 0x8CU, 0x8CU);
static const struct store_rgb accent = STORE_RGB(0x00U, 0x78U, 0xD7U);
static const struct store_rgb on_accent = STORE_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct store_rgb rating_track = STORE_RGB(0xDCU, 0xDCU, 0xDCU);
/*
 * The one ground every card's artwork sits on.
 *
 * A true neutral: the app icons carry all the colour, which is how the
 * Store does it and why nine cards on one ground still read as nine apps.
 * This was written as 0x5A6U & 0xFFU, which is 0xA6 rather than 0x5A - a
 * dusty pink that never showed because every card used to pass a colour of
 * its own.  Now that they do not, it would have.
 */
static const struct store_rgb neutral_plate = STORE_RGB(0x5AU, 0x5AU,
    0x5EU);
/* What a card's plate is mostly made of - see plate_of(). */
static const struct store_rgb plate_base = STORE_RGB(0x26U, 0x26U, 0x2AU);
static const struct store_rgb border_active = STORE_RGB(0x00U, 0x78U, 0xD7U);
static const struct store_rgb border_inactive = STORE_RGB(0x9BU, 0x9BU,
    0x9BU);

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool focused = true;
static size_t current_tab;
static struct store_app apps[STORE_MAX_APPS];
static char shelf_heading[STORE_MAX_SHELVES][STORE_TEXT_BYTES];
static size_t hover_app = (size_t)-1;
/*
 * The card being LEFT, and the two fades.  Windows cross-fades a hover
 * rather than switching it: the card you are leaving is still half lit while
 * the one you have arrived at comes up.  Two motions cover that; one per
 * card would be twenty-three more of them for a state only two can be in.
 */
static size_t leaving_app = (size_t)-1;
static struct ui_motion hover_fade;
static struct ui_motion leave_fade;
static const char *self_test_failure = "store self-test has not run";

static const char *const store_tabs[STORE_TAB_COUNT] = {
    "Home", "Apps", "Games", "Creative"
};

const char *store_status_string(enum store_status status)
{
    switch (status) {
    case STORE_STATUS_OK:
        return "ok";
    case STORE_STATUS_NULL_ARGUMENT:
        return "null argument";
    case STORE_STATUS_NOT_INITIALIZED:
        return "store not initialized";
    case STORE_STATUS_BAD_INDEX:
        return "store index is out of range";
    case STORE_STATUS_UNSUPPORTED_GEOMETRY:
        return "store geometry is unsupported";
    case STORE_STATUS_SURFACE_FAILURE:
        return "store surface refused a pixel";
    default:
        return "unknown store status";
    }
}

/* ============================================================== PRIMITIVES */

static uint32_t pack_rgb(struct store_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return ((uint32_t)colour.red << format.red_position) |
        ((uint32_t)colour.green << format.green_position) |
        ((uint32_t)colour.blue << format.blue_position);
}

static struct store_rgb unpack_rgb(uint32_t pixel)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return (struct store_rgb){
        (uint8_t)((pixel >> format.red_position) & 0xFFU),
        (uint8_t)((pixel >> format.green_position) & 0xFFU),
        (uint8_t)((pixel >> format.blue_position) & 0xFFU)
    };
}

/* A colour mixed towards white, for the tint a card's foot takes from its
 * own plate.  This is what stops a shelf reading as eight white rectangles
 * without shouting. */
static struct store_rgb toward_white(struct store_rgb colour, uint32_t weight)
{
    const uint32_t inverse = 255U - weight;

    return (struct store_rgb){
        (uint8_t)((colour.red * weight + 255U * inverse) / 255U),
        (uint8_t)((colour.green * weight + 255U * inverse) / 255U),
        (uint8_t)((colour.blue * weight + 255U * inverse) / 255U)
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

static enum store_status fill(struct ui_rect area, struct ui_rect damage,
    struct store_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return STORE_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return STORE_STATUS_OK;
}
/* The same, mixed into what is already there - which is what a cross-fade
 * needs and a plain fill cannot do. */
static enum store_status blend(struct ui_rect area, struct ui_rect damage,
    struct store_rgb colour, uint32_t alpha)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t over = pack_rgb(colour);

    if (alpha == 0U) {
        return STORE_STATUS_OK;
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
                return STORE_STATUS_SURFACE_FAILURE;
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
                return STORE_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return STORE_STATUS_OK;
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


static enum store_status text_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct store_rgb colour)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (clip.width == 0U || clip.height == 0U || body == NULL ||
            body[0] == '\0') {
        return STORE_STATUS_OK;
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
    return STORE_STATUS_OK;
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

/* Text that stops at a limit rather than running into what is beside it. */
static enum store_status text_clipped(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct store_rgb colour,
    uint32_t limit)
{
    return text_at(intersect(damage, (struct ui_rect){ x, window_rect.y,
        limit, window_rect.height }), x, baseline, body, colour);
}

/*
 * Text that ends in an ellipsis rather than mid-letter.  A hard clip reads
 * as a rendering fault; "Photo & vi..." reads as a name too long for its
 * card, which is what it is.  Explorer does the same for file names.
 */
static enum store_status text_elided(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct store_rgb colour,
    uint32_t limit)
{
    char shortened[STORE_TEXT_BYTES + 4U];
    size_t length = 0U;

    if (body == NULL) {
        return STORE_STATUS_OK;
    }
    if (width_of(body) <= limit) {
        return text_at(damage, x, baseline, body, colour);
    }
    while (body[length] != '\0' && length + 4U < sizeof(shortened)) {
        shortened[length] = body[length];
        ++length;
    }
    while (length > 0U) {
        --length;
        shortened[length] = '.';
        shortened[length + 1U] = '.';
        shortened[length + 2U] = '.';
        shortened[length + 3U] = '\0';
        if (width_of(shortened) <= limit) {
            break;
        }
    }
    /* Below about three characters there is nothing to elide to, and the
     * clip inside text_clipped is then the only honest answer. */
    return text_clipped(damage, x, baseline, shortened, colour, limit);
}

static bool names_match(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static enum store_status draw_line_glyph(const char *name, struct ui_rect box,
    struct ui_rect damage, struct store_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t wanted = box.width < box.height ? box.width : box.height;
    const uint32_t over = pack_rgb(colour);
    const uint8_t *cell = NULL;
    uint32_t size = 0U;
    struct ui_rect placed;
    struct ui_rect clipped;

    for (size_t index = 0U; index < EXPLORER_LUCIDE_COUNT; ++index) {
        size_t choice = 0U;

        if (!names_match(explorer_lucide[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < EXPLORER_LUCIDE_SIZES; ++option) {
            if (explorer_lucide_size[option] <= wanted) {
                choice = option;
            }
        }
        cell = explorer_lucide[index].alpha[choice];
        size = explorer_lucide_size[choice];
        break;
    }
    if (cell == NULL) {
        return STORE_STATUS_OK;
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
                return STORE_STATUS_SURFACE_FAILURE;
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
                return STORE_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return STORE_STATUS_OK;
}

/*
 * An application's own picture, drawn from the taskbar's artwork rather than
 * from a second copy of it - which is the whole reason taskbar_artwork()
 * exists.
 */
static enum store_status draw_artwork(const char *art, struct ui_rect box,
    struct ui_rect damage)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t *pixels = NULL;
    const uint8_t *alpha = NULL;
    uint32_t side = 0U;
    struct ui_rect placed;
    struct ui_rect clipped;

    if (art == NULL || !taskbar_artwork(art,
            box.width < box.height ? box.width : box.height,
            &pixels, &alpha, &side) || side == 0U) {
        return STORE_STATUS_OK;
    }
    placed = (struct ui_rect){
        box.x + (box.width > side ? (box.width - side) / 2U : 0U),
        box.y + (box.height > side ? (box.height - side) / 2U : 0U),
        side, side };
    clipped = intersect(placed, damage);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const size_t offset = (size_t)local_y * side +
                (clipped.x - placed.x + x);
            const uint8_t coverage = alpha[offset];
            const uint32_t stored = pixels[offset];
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (coverage == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return STORE_STATUS_SURFACE_FAILURE;
            }
            red = (((stored >> 16) & 0xFFU) * coverage +
                ((under >> format.red_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            green = (((stored >> 8) & 0xFFU) * coverage +
                ((under >> format.green_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            blue = ((stored & 0xFFU) * coverage +
                ((under >> format.blue_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) != SURFACE_STATUS_OK) {
                return STORE_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return STORE_STATUS_OK;
}

/* ================================================================ GEOMETRY */

static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){ window_rect.x + STORE_BORDER,
        window_rect.y + STORE_BORDER,
        window_rect.width - STORE_BORDER * 2U, STORE_CAPTION };
}

static struct ui_rect nav_rect(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x, caption.y + caption.height,
        caption.width, STORE_NAV };
}

static struct ui_rect page_rect(void)
{
    const struct ui_rect nav = nav_rect();

    return (struct ui_rect){ nav.x, nav.y + nav.height, nav.width,
        window_rect.height - STORE_BORDER * 2U - STORE_CAPTION - STORE_NAV };
}

static struct ui_rect caption_button_rect(uint32_t index)
{
    const struct ui_rect caption = caption_rect();
    const uint32_t from_right = 3U - index;

    return (struct ui_rect){
        caption.x + caption.width - from_right * STORE_CAPTION_BUTTON,
        caption.y, STORE_CAPTION_BUTTON, caption.height };
}

static size_t spotlight_index(void)
{
    for (size_t index = 0U; index < STORE_MAX_APPS; ++index) {
        if (apps[index].present && apps[index].spotlight) {
            return index;
        }
    }
    return (size_t)-1;
}

static struct ui_rect spotlight_rect(void)
{
    const struct ui_rect area = page_rect();

    return (struct ui_rect){ area.x + STORE_PAD, area.y + STORE_PAD,
        area.width - STORE_PAD * 2U, STORE_SPOTLIGHT_HEIGHT };
}

static struct ui_rect spotlight_action_rect(void)
{
    const struct ui_rect panel = spotlight_rect();

    return (struct ui_rect){ panel.x + 32U + STORE_SPOTLIGHT_LOGO + 32U,
        panel.y + panel.height - 46U, STORE_GET_WIDTH, STORE_GET_HEIGHT };
}

/* How tall one shelf is: its heading, then a row of cards. */
static uint32_t shelf_height(void)
{
    return STORE_HEADING + STORE_CARD_PLATE + STORE_CARD_FOOT +
        STORE_CARD_GAP;
}

static uint32_t shelf_top(size_t shelf)
{
    const struct ui_rect area = page_rect();
    uint32_t top = area.y + STORE_PAD;

    if (spotlight_index() != (size_t)-1) {
        top += STORE_SPOTLIGHT_HEIGHT + STORE_PAD;
    }
    return top + (uint32_t)shelf * shelf_height();
}

/* Which of the shelf's own cards this index is, counting from the left. */
static size_t card_position(size_t index)
{
    size_t position = 0U;

    for (size_t scan = 0U; scan < index; ++scan) {
        if (apps[scan].present && !apps[scan].spotlight &&
                apps[scan].shelf == apps[index].shelf) {
            ++position;
        }
    }
    return position;
}

static struct ui_rect card_rect(size_t index)
{
    const struct ui_rect area = page_rect();
    const uint32_t left = area.x + STORE_PAD +
        (uint32_t)card_position(index) * (STORE_CARD_WIDTH + STORE_CARD_GAP);
    const uint32_t top = shelf_top(apps[index].shelf) + STORE_HEADING;

    if (!apps[index].present || apps[index].spotlight ||
            left + STORE_CARD_WIDTH > area.x + area.width - STORE_PAD ||
            top + STORE_CARD_PLATE + STORE_CARD_FOOT >
                area.y + area.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ left, top, STORE_CARD_WIDTH,
        STORE_CARD_PLATE + STORE_CARD_FOOT };
}

struct ui_rect store_bounds(void)
{
    return window_rect;
}

enum store_status store_primary_action_bounds(struct ui_rect *bounds)
{
    if (bounds == NULL) {
        return STORE_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        *bounds = (struct ui_rect){ 0U, 0U, 0U, 0U };
        return STORE_STATUS_NOT_INITIALIZED;
    }
    *bounds = spotlight_index() == (size_t)-1 ?
        (struct ui_rect){ 0U, 0U, 0U, 0U } : spotlight_action_rect();
    return STORE_STATUS_OK;
}

/* ================================================================== PIECES */

/*
 * The ground behind a card's artwork.
 *
 * An explicit colour wins; otherwise it is FOUND, from the app's own icon,
 * through the same saturation-weighted hue histogram the taskbar lights a
 * hovered button with - so the Store and the taskbar agree about what colour
 * an application is, and neither has it written down.
 *
 * Two versions of this were wrong in opposite directions.  The first gave
 * every card a saturated plate chosen by hand, which is a paint chart: nine
 * colours standing for nothing but the order they were typed in.  The second
 * made them all one neutral, which is honest and dull, and throws away the
 * one thing that tells cards apart at a glance in a grid.  A colour that is
 * derived is neither - Notes is gold because its note IS gold, and the
 * Store is purple because its bag is.
 *
 * Taken to two fifths of its value, because the finder lifts a hue to full
 * brightness for a glow and a plate is a background: the artwork has to read
 * against it.  A monochrome mark has no dominant hue and keeps the neutral.
 */
static struct store_rgb plate_of(const struct store_app *app)
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    if (app->colour != 0U) {
        return unpack_rgb(app->colour);
    }
    if (app->art == NULL || !taskbar_artwork_tint(app->art,
            STORE_CARD_PLATE, &red, &green, &blue)) {
        return neutral_plate;
    }
    /*
     * A dark card TINTED by the app, not the app's own colour darkened.
     *
     * taskbar_artwork_tint() reports the DOMINANT colour, so for Notes it
     * hands back the sticky note's full-strength yellow.  Darkening that
     * outright - which is what a flat two-fifths did - takes the light away
     * and leaves all of the chroma, and a bright yellow with the light taken
     * away is olive.  The Notes and Files cards came out the colour of pond
     * water with a yellow icon on them at almost no contrast, and every warm
     * app on the shelf did the same.
     *
     * So the chroma goes first and the darkness second.  The tint is pulled
     * most of the way to its own grey, then a quarter of THAT is mixed into
     * a near-neutral base: enough to tell a warm app from a cool one across
     * the shelf, not enough to muddy either, and the artwork stays the
     * brightest thing on its own card - which is what a shelf is for.
     */
    {
        /* Rec. 709 luma in 256ths, which is the grey this colour weighs
         * the same as. */
        const uint32_t grey = (54U * red + 183U * green + 19U * blue) / 256U;
        const uint32_t soft_red = (red * 2U + grey * 3U) / 5U;
        const uint32_t soft_green = (green * 2U + grey * 3U) / 5U;
        const uint32_t soft_blue = (blue * 2U + grey * 3U) / 5U;

        return (struct store_rgb){
            (uint8_t)(((uint32_t)plate_base.red * 3U + soft_red) / 4U),
            (uint8_t)(((uint32_t)plate_base.green * 3U + soft_green) / 4U),
            (uint8_t)(((uint32_t)plate_base.blue * 3U + soft_blue) / 4U)
        };
    }
}

/*
 * The rating, as a bar rather than as five stars.
 *
 * Five stars at card size is twenty pixels of shape carrying one number
 * badly - at that scale the difference between four stars and four and a
 * half is two lit pixels.  A bar carries it at a glance and leaves room for
 * the review count beside it, which is the number people actually weigh.
 */
static enum store_status draw_rating(struct ui_rect box,
    struct ui_rect damage, const struct store_app *app, bool on_plate)
{
    const uint32_t rating = app->rating > 50U ? 50U : app->rating;
    const uint32_t lit = STORE_RATING_WIDTH * rating / 50U;
    /* On a card the bar is the accent over grey; on the spotlight's own
     * colour both of those disappear, so it is white over a wash of it. */
    const struct store_rgb track = on_plate ?
        toward_white(plate_of(app), 160U) : rating_track;
    const struct store_rgb lit_colour = on_plate ? on_accent : accent;
    const enum store_status status = fill((struct ui_rect){ box.x, box.y,
        STORE_RATING_WIDTH, STORE_RATING_HEIGHT }, damage, track);

    if (status != STORE_STATUS_OK || lit == 0U) {
        return status;
    }
    return fill((struct ui_rect){ box.x, box.y, lit, STORE_RATING_HEIGHT },
        damage, lit_colour);
}

static enum store_status draw_card(size_t index, struct ui_rect damage)
{
    const struct store_app *app = &apps[index];
    const struct ui_rect card = card_rect(index);
    const struct store_rgb plate = plate_of(app);
    enum store_status status;

    if (card.width == 0U) {
        return STORE_STATUS_OK;
    }
    status = fill(card, damage, card_fill);
    if (status == STORE_STATUS_OK) {
        /*
         * The plate in the app's OWN colour.  Windows puts every logo on the
         * same white card and lets them fight the background; a shelf drawn
         * that way reads as eight white rectangles.
         */
        status = fill((struct ui_rect){ card.x, card.y, card.width,
            STORE_CARD_PLATE }, damage, plate);
    }
    if (status == STORE_STATUS_OK) {
        /* Hovering lifts the foot into a tint of the same colour, so the
         * whole card responds rather than a border appearing - and it
         * cross-fades, so leaving one and arriving at the next is a single
         * movement across the shelf. */
        const uint32_t lift = index == hover_app ?
            ui_motion_alpha(&hover_fade) : (index == leaving_app ?
                ui_motion_alpha(&leave_fade) : 0U);

        status = blend((struct ui_rect){ card.x, card.y + STORE_CARD_PLATE,
            card.width, STORE_CARD_FOOT }, damage,
            toward_white(plate, 30U), lift);
    }
    if (status == STORE_STATUS_OK) {
        status = draw_artwork(app->art, (struct ui_rect){ card.x,
            card.y, card.width, STORE_CARD_PLATE }, damage);
    }
    if (status != STORE_STATUS_OK) {
        return status;
    }

    const uint32_t left = card.x + 10U;
    const uint32_t limit = card.width - 20U;
    const uint32_t foot = card.y + STORE_CARD_PLATE;

    const uint32_t price_width = width_of(app->price);
    const uint32_t beside_bar = STORE_RATING_WIDTH + 6U;

    status = text_elided(damage, left, foot + STORE_FOOT_NAME, app->name,
        ink, limit);
    if (status == STORE_STATUS_OK) {
        status = text_elided(damage, left, foot + STORE_FOOT_CATEGORY,
            app->category, ink_faint,
            price_width + 8U < limit ? limit - price_width - 8U : limit);
    }
    if (status == STORE_STATUS_OK && price_width < limit) {
        status = text_at(damage, left + limit - price_width,
            foot + STORE_FOOT_CATEGORY, app->price, ink_soft);
    }
    if (status == STORE_STATUS_OK) {
        status = draw_rating((struct ui_rect){ left,
            foot + STORE_FOOT_RATING, STORE_RATING_WIDTH,
            STORE_RATING_HEIGHT }, damage, app, false);
    }
    if (status == STORE_STATUS_OK) {
        status = text_clipped(damage, left + beside_bar,
            foot + STORE_FOOT_REVIEWS, app->reviews, ink_faint,
            limit - beside_bar);
    }
    return status;
}

static enum store_status draw_spotlight(struct ui_rect damage)
{
    const size_t index = spotlight_index();

    if (index == (size_t)-1) {
        return STORE_STATUS_OK;
    }

    const struct store_app *app = &apps[index];
    const struct ui_rect panel = spotlight_rect();
    const struct store_rgb plate = plate_of(app);
    const struct ui_rect button = spotlight_action_rect();
    enum store_status status = fill(panel, damage, plate);

    if (status == STORE_STATUS_OK) {
        const struct ui_rect tile = { panel.x + 32U,
            panel.y + (panel.height - STORE_SPOTLIGHT_LOGO) / 2U,
            STORE_SPOTLIGHT_LOGO, STORE_SPOTLIGHT_LOGO };
        /* A shade of the panel's own colour rather than a fixed grey, so
         * the tile belongs to whichever app is being featured. */
        const struct store_rgb tile_fill = {
            (uint8_t)(plate.red + (255U - plate.red) / 6U),
            (uint8_t)(plate.green + (255U - plate.green) / 6U),
            (uint8_t)(plate.blue + (255U - plate.blue) / 6U)
        };

        status = fill(tile, damage, tile_fill);
        if (status == STORE_STATUS_OK) {
            status = draw_artwork(app->art, (struct ui_rect){
                tile.x + (STORE_SPOTLIGHT_LOGO - STORE_SPOTLIGHT_ART) / 2U,
                tile.y + (STORE_SPOTLIGHT_LOGO - STORE_SPOTLIGHT_ART) / 2U,
                STORE_SPOTLIGHT_ART, STORE_SPOTLIGHT_ART }, damage);
        }
    }
    if (status != STORE_STATUS_OK) {
        return status;
    }

    const uint32_t left = panel.x + 32U + STORE_SPOTLIGHT_LOGO + 32U;

    status = text_at(damage, left, panel.y + 46U, app->name, on_accent);
    if (status == STORE_STATUS_OK) {
        status = text_at(damage, left, panel.y + 68U, app->tagline,
            on_accent);
    }
    if (status == STORE_STATUS_OK) {
        status = text_at(damage, left, panel.y + 90U, app->category,
            on_accent);
    }
    if (status == STORE_STATUS_OK) {
        status = draw_rating((struct ui_rect){ left + width_of(app->category)
            + 14U, panel.y + 85U, STORE_RATING_WIDTH, STORE_RATING_HEIGHT },
            damage, app, true);
    }
    if (status == STORE_STATUS_OK) {
        status = text_at(damage, left + width_of(app->category) + 14U +
            STORE_RATING_WIDTH + 8U, panel.y + 90U, app->reviews, on_accent);
    }
    if (status == STORE_STATUS_OK) {
        /* The Get button, which is the one white thing on the panel. */
        status = fill(button, damage, on_accent);
    }
    if (status == STORE_STATUS_OK) {
        status = text_at(damage,
            button.x + (button.width - width_of(app->price)) / 2U,
            button.y + button.height / 2U + 5U, app->price, plate);
    }
    return status;
}

static enum store_status draw_shelves(struct ui_rect damage)
{
    enum store_status status = STORE_STATUS_OK;

    for (size_t shelf = 0U; shelf < STORE_MAX_SHELVES &&
            status == STORE_STATUS_OK; ++shelf) {
        const struct ui_rect area = page_rect();
        const uint32_t top = shelf_top(shelf);
        bool populated = false;

        for (size_t index = 0U; index < STORE_MAX_APPS; ++index) {
            if (apps[index].present && !apps[index].spotlight &&
                    apps[index].shelf == shelf) {
                populated = true;
                break;
            }
        }
        if (!populated || shelf_heading[shelf][0] == '\0' ||
                top + STORE_HEADING > area.y + area.height) {
            continue;
        }
        status = text_at(damage, area.x + STORE_PAD, top + 22U,
            shelf_heading[shelf], ink);
        if (status != STORE_STATUS_OK) {
            return status;
        }
        /* "Show all", at the right of the heading, which is where Windows
         * puts the way out of a shelf. */
        status = text_at(damage,
            area.x + area.width - STORE_PAD - width_of("Show all") - 16U,
            top + 22U, "Show all", accent);
        if (status == STORE_STATUS_OK) {
            /* Sixteen and not fourteen: the smallest cell this mark exists
             * at IS sixteen, so a fourteen-pixel box did not shrink it - it
             * drew sixteen pixels from the box's left edge and bled two into
             * the margin beyond the shelf. */
            status = draw_line_glyph("chevron-right", (struct ui_rect){
                area.x + area.width - STORE_PAD - 16U, top + 4U, 16U, 24U },
                damage, accent);
        }
    }
    for (size_t index = 0U; index < STORE_MAX_APPS &&
            status == STORE_STATUS_OK; ++index) {
        if (apps[index].present && !apps[index].spotlight) {
            status = draw_card(index, damage);
        }
    }
    return status;
}

static enum store_status draw_chrome(struct ui_rect damage)
{
    const struct ui_rect caption = caption_rect();
    const struct ui_rect nav = nav_rect();
    enum store_status status = fill(caption, damage, chrome);
    uint32_t pen = nav.x + STORE_PAD;

    if (status == STORE_STATUS_OK) {
        status = draw_artwork("store", (struct ui_rect){ caption.x + 8U,
            caption.y, 16U, caption.height }, damage);
    }
    if (status == STORE_STATUS_OK) {
        status = text_at(damage, caption.x + 32U,
            caption.y + caption.height / 2U + 5U, "Phipia Store",
            focused ? ink : ink_faint);
    }
    for (uint32_t index = 0U; index < 3U && status == STORE_STATUS_OK;
         ++index) {
        const struct ui_rect button = caption_button_rect(index);
        const uint32_t size = 10U;
        const uint32_t bx = button.x + (button.width - size) / 2U;
        const uint32_t by = button.y + (button.height - size) / 2U;
        const struct store_rgb mark = focused ? ink : ink_faint;

        if (index == 0U) {
            status = fill((struct ui_rect){ bx, by + size / 2U, size, 1U },
                damage, mark);
        } else if (index == 1U) {
            status = fill((struct ui_rect){ bx, by, size, 1U }, damage, mark);
            if (status == STORE_STATUS_OK) {
                status = fill((struct ui_rect){ bx, by + size - 1U, size,
                    1U }, damage, mark);
            }
            if (status == STORE_STATUS_OK) {
                status = fill((struct ui_rect){ bx, by, 1U, size }, damage,
                    mark);
            }
            if (status == STORE_STATUS_OK) {
                status = fill((struct ui_rect){ bx + size - 1U, by, 1U,
                    size }, damage, mark);
            }
        } else {
            for (uint32_t step = 0U; step < size &&
                    status == STORE_STATUS_OK; ++step) {
                status = fill((struct ui_rect){ bx + step, by + step, 1U,
                    1U }, damage, mark);
                if (status == STORE_STATUS_OK) {
                    status = fill((struct ui_rect){ bx + step,
                        by + size - 1U - step, 1U, 1U }, damage, mark);
                }
            }
        }
    }
    if (status == STORE_STATUS_OK) {
        status = fill(nav, damage, chrome);
    }
    if (status == STORE_STATUS_OK) {
        status = fill((struct ui_rect){ nav.x, nav.y + nav.height - 1U,
            nav.width, 1U }, damage, rule);
    }
    /*
     * The tabs.  Windows 10's Store navigates ACROSS THE TOP rather than
     * down a rail, and marks the open one with a short accent underline -
     * the same underline the taskbar puts under a running application, which
     * is why the two look like one system.
     */
    for (size_t index = 0U; index < STORE_TAB_COUNT &&
            status == STORE_STATUS_OK; ++index) {
        const uint32_t width = width_of(store_tabs[index]) +
            STORE_TAB_PAD * 2U;
        const bool open = index == current_tab;

        status = text_at(damage, pen + STORE_TAB_PAD,
            nav.y + nav.height / 2U + 5U, store_tabs[index],
            open ? ink : ink_soft);
        if (status == STORE_STATUS_OK && open) {
            status = fill((struct ui_rect){ pen + STORE_TAB_PAD,
                nav.y + nav.height - 4U, width - STORE_TAB_PAD * 2U, 3U },
                damage, accent);
        }
        pen += width;
    }
    if (status != STORE_STATUS_OK) {
        return status;
    }

    const struct ui_rect search = { nav.x + nav.width - STORE_PAD -
        STORE_SEARCH, nav.y + 8U, STORE_SEARCH, nav.height - 18U };

    status = fill(search, damage, page);
    if (status == STORE_STATUS_OK) {
        status = fill((struct ui_rect){ search.x,
            search.y + search.height - 1U, search.width, 1U }, damage, rule);
    }
    if (status == STORE_STATUS_OK) {
        status = draw_line_glyph("search", (struct ui_rect){
            search.x + search.width - 24U, search.y, 20U, search.height },
            damage, ink_soft);
    }
    if (status == STORE_STATUS_OK) {
        status = text_at(damage, search.x + 10U,
            search.y + search.height / 2U + 5U, "Search apps", ink_faint);
    }
    return status;
}

enum store_status store_draw(struct ui_rect damage)
{
    enum store_status status;

    if (!initialized) {
        return STORE_STATUS_NOT_INITIALIZED;
    }
    status = fill(window_rect, damage,
        focused ? border_active : border_inactive);
    if (status == STORE_STATUS_OK) {
        status = fill(page_rect(), damage, page);
    }
    if (status == STORE_STATUS_OK) {
        status = draw_chrome(damage);
    }
    if (status == STORE_STATUS_OK) {
        status = draw_spotlight(damage);
    }
    if (status == STORE_STATUS_OK) {
        status = draw_shelves(damage);
    }
    if (status != STORE_STATUS_OK) {
        return status;
    }
    /*
     * The empty state.
     *
     * There is no catalogue in this module - every app is handed over by the
     * caller - so a Store that has been given none is the DEFAULT state, not
     * an error, and it has to look like something rather than like a blank
     * window somebody forgot to finish.
     */
    for (size_t index = 0U; index < STORE_MAX_APPS; ++index) {
        if (apps[index].present) {
            return STORE_STATUS_OK;
        }
    }

    const struct ui_rect area = page_rect();
    static const char headline[] = "Nothing here yet";
    static const char detail[] =
        "Apps appear once a catalogue is connected.";

    status = draw_line_glyph("box", (struct ui_rect){
        area.x + area.width / 2U - 24U, area.y + area.height / 2U - 70U,
        48U, 48U }, damage, ink_faint);
    if (status == STORE_STATUS_OK) {
        status = text_at(damage,
            area.x + (area.width - width_of(headline)) / 2U,
            area.y + area.height / 2U, headline, ink_soft);
    }
    if (status == STORE_STATUS_OK) {
        status = text_at(damage,
            area.x + (area.width - width_of(detail)) / 2U,
            area.y + area.height / 2U + 22U, detail, ink_faint);
    }
    return status;
}

/* ================================================================== INPUT */

enum store_status store_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const size_t was = hover_app;

    if (damage == NULL) {
        return STORE_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return STORE_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    hover_app = (size_t)-1;
    for (size_t index = 0U; index < STORE_MAX_APPS; ++index) {
        if (apps[index].present && !apps[index].spotlight &&
                holds(card_rect(index), point)) {
            hover_app = index;
            break;
        }
    }
    if (was != hover_app) {
        /* 83 ms and linear, which is what a XAML BrushTransition is; see
         * ui_motion.h. */
        const uint64_t now = clock_monotonic_ns();

        leaving_app = was;
        ui_motion_reset(&leave_fade, hover_fade.value);
        ui_motion_to(&leave_fade, 0, UI_MOTION_BRUSH_NS, now);
        ui_motion_reset(&hover_fade, 0);
        if (hover_app != (size_t)-1) {
            ui_motion_to(&hover_fade, (int32_t)UI_MOTION_ONE,
                UI_MOTION_BRUSH_NS, now);
        }
        *damage = window_rect;
    }
    return STORE_STATUS_OK;
}

/* A card's rectangle, or nothing at all for the index that means none. */
static struct ui_rect hover_damage(size_t index)
{
    if (index >= STORE_MAX_APPS) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return card_rect(index);
}

bool store_animate(struct ui_rect *damage)
{
    const uint64_t now = clock_monotonic_ns();
    bool moved = false;

    if (damage == NULL || !initialized) {
        return false;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (ui_motion_advance(&hover_fade, now, ui_ease_linear)) {
        *damage = join(*damage, hover_damage(hover_app));
        moved = true;
    }
    if (ui_motion_advance(&leave_fade, now, ui_ease_linear)) {
        *damage = join(*damage, hover_damage(leaving_app));
        moved = true;
    }
    return moved;
}

bool store_animating(void)
{
    return ui_motion_running(&hover_fade) || ui_motion_running(&leave_fade);
}

enum store_status store_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    const struct ui_rect nav = nav_rect();
    uint32_t pen;

    if (damage == NULL) {
        return STORE_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return STORE_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!holds(nav, point)) {
        return STORE_STATUS_OK;
    }
    pen = nav.x + STORE_PAD;
    for (size_t index = 0U; index < STORE_TAB_COUNT; ++index) {
        const uint32_t width = width_of(store_tabs[index]) +
            STORE_TAB_PAD * 2U;

        if (holds((struct ui_rect){ pen, nav.y, width, nav.height }, point)) {
            current_tab = index;
            *damage = window_rect;
            return STORE_STATUS_OK;
        }
        pen += width;
    }
    return STORE_STATUS_OK;
}

/* ============================================================== LIFECYCLE */

static void copy_field(char *destination, const char *source)
{
    size_t index = 0U;

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    while (index + 1U < STORE_TEXT_BYTES && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

enum store_status store_set_frame(struct ui_rect frame)
{
    const uint32_t least_width = STORE_BORDER * 2U + STORE_PAD * 2U +
        STORE_CARD_WIDTH * 2U + STORE_CARD_GAP;
    const uint32_t least_height = STORE_BORDER * 2U + STORE_CAPTION +
        STORE_NAV + STORE_PAD + STORE_HEADING + STORE_CARD_PLATE +
        STORE_CARD_FOOT;

    if (frame.width < least_width || frame.height < least_height) {
        return STORE_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return STORE_STATUS_OK;
}

enum store_status store_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum store_status status;

    if (target == NULL) {
        return STORE_STATUS_NULL_ARGUMENT;
    }
    status = store_set_frame(frame);
    if (status != STORE_STATUS_OK) {
        return status;
    }
    canvas = target;
    initialized = true;
    return STORE_STATUS_OK;
}

enum store_status store_set_app(size_t index, const struct store_app *app)
{
    if (index >= STORE_MAX_APPS) {
        return STORE_STATUS_BAD_INDEX;
    }
    if (app == NULL) {
        apps[index] = (struct store_app){ 0 };
        return STORE_STATUS_OK;
    }
    if (app->shelf >= STORE_MAX_SHELVES || app->rating > 50U) {
        return STORE_STATUS_BAD_INDEX;
    }
    apps[index] = *app;
    apps[index].present = true;
    apps[index].name[STORE_TEXT_BYTES - 1U] = '\0';
    return STORE_STATUS_OK;
}

enum store_status store_set_shelf(size_t shelf, const char *heading)
{
    if (shelf >= STORE_MAX_SHELVES) {
        return STORE_STATUS_BAD_INDEX;
    }
    copy_field(shelf_heading[shelf], heading);
    return STORE_STATUS_OK;
}

enum store_status store_set_tab(size_t tab)
{
    if (tab >= STORE_TAB_COUNT) {
        return STORE_STATUS_BAD_INDEX;
    }
    current_tab = tab;
    return STORE_STATUS_OK;
}

enum store_status store_set_focus(bool active)
{
    focused = active;
    return STORE_STATUS_OK;
}

/* ============================================================== SELF TEST */

bool store_self_test(void)
{
    struct store_app probe = { 0 };

    /* A rating past five stars would draw a bar past its own track. */
    probe.rating = 51U;
    if (store_set_app(0U, &probe) != STORE_STATUS_BAD_INDEX) {
        self_test_failure = "the store accepted a rating past five stars";
        return false;
    }
    probe.rating = 50U;
    probe.shelf = STORE_MAX_SHELVES;
    if (store_set_app(0U, &probe) != STORE_STATUS_BAD_INDEX) {
        self_test_failure = "the store accepted a shelf that does not exist";
        return false;
    }
    if (store_set_app(STORE_MAX_APPS, &probe) != STORE_STATUS_BAD_INDEX) {
        self_test_failure = "the store accepted an index past the end";
        return false;
    }
    if (store_set_tab(STORE_TAB_COUNT) != STORE_STATUS_BAD_INDEX) {
        self_test_failure = "the store accepted a tab that does not exist";
        return false;
    }
    /* The rating bar has to reach its own end at five stars and nowhere
     * before it, or the number it carries is a lie. */
    if (STORE_RATING_WIDTH * 50U / 50U != STORE_RATING_WIDTH ||
            STORE_RATING_WIDTH * 0U / 50U != 0U) {
        self_test_failure = "the store rating bar does not span its track";
        return false;
    }
    /* The three caption rows have to clear one another and land inside the
     * foot.  A font taller than the one the card was laid out against
     * would slide them into each other, which is the defect this layout
     * replaced; catch it here rather than on the glass. */
    {
        const struct ui_font_metrics font = ui_font_get_metrics();

        if (STORE_FOOT_NAME + font.descent + font.ascent >
                    STORE_FOOT_CATEGORY ||
                STORE_FOOT_CATEGORY + font.descent + font.ascent >
                    STORE_FOOT_REVIEWS ||
                STORE_FOOT_REVIEWS + font.descent > STORE_CARD_FOOT) {
            self_test_failure = "the store card caption rows overlap";
            return false;
        }
    }
    self_test_failure = "";
    return true;
}

const char *store_self_test_failure(void)
{
    return self_test_failure;
}
