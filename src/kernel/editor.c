/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Editor.  See include/phipia/editor.h for the shape and for what it
 * deliberately leaves out.
 */

#include <phipia/editor.h>

#include <phipia/clock.h>
#include <phipia/framebuffer.h>
#include <phipia/ui_font.h>

#include "editor_glyphs.h"
#include "ui_motion.h"

/* ================================================================ METRICS
 *
 * The arrangement is CapCut's; the numbers are this file's, chosen so the
 * window fits the 1280x800 canvas above a 40-pixel bar with room round it.
 *
 * Every box that holds a glyph is an even number of pixels larger than the
 * glyph it holds, so the centring comes out whole - a hinted stroke landed
 * on a half pixel is a smeared stroke.
 */

#define EDITOR_BORDER 1U
#define EDITOR_CAPTION 32U
#define EDITOR_CAPTION_BUTTON 46U
#define EDITOR_CAPTION_MARK 10U

#define EDITOR_WIDTH 1000U
#define EDITOR_HEIGHT 620U

/* The library, down the left: a tab strip over a grid of presets. */
#define EDITOR_LIBRARY 208U
#define EDITOR_TAB_HEIGHT 40U
#define EDITOR_TAB_GLYPH 16U
#define EDITOR_PRESET_HEIGHT 44U
#define EDITOR_PRESET_PAD 10U

/* The properties panel, down the right. */
#define EDITOR_PROPERTIES 224U
#define EDITOR_ROW_HEIGHT 46U
#define EDITOR_FIELD_HEIGHT 26U
#define EDITOR_SLIDER_TRACK 3U
#define EDITOR_SLIDER_THUMB 10U

/* The player: a stage with the transport under it. */
#define EDITOR_TRANSPORT 44U
#define EDITOR_TRANSPORT_BUTTON 32U
#define EDITOR_TRANSPORT_GLYPH 20U
#define EDITOR_SCRUB_TRACK 3U

/* The timeline, across the bottom. */
#define EDITOR_TIMELINE 168U
#define EDITOR_RULER 22U
#define EDITOR_LANE_HEIGHT 42U
#define EDITOR_GUTTER 76U
#define EDITOR_TIMELINE_PAD 12U
#define EDITOR_PLAYHEAD_GRIP 7U

/* What a preset drops on the timeline, and the least it can be trimmed to. */
#define EDITOR_NEW_LENGTH_MS 2500U
#define EDITOR_LEAST_LENGTH_MS 250U

/* ================================================================ PALETTE
 *
 * Dark, because an editor's picture is the only thing in the window that
 * should be bright, and Windows 10's own Video Editor was dark for the same
 * reason.  The accent is the shell's, so the window belongs to Phipia even
 * though its surface is nothing like a Settings page.
 */

struct editor_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define EDITOR_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct editor_rgb caption_fill = EDITOR_RGB(0x1FU, 0x1FU, 0x1FU);
static const struct editor_rgb panel = EDITOR_RGB(0x25U, 0x25U, 0x25U);
static const struct editor_rgb panel_deep = EDITOR_RGB(0x1AU, 0x1AU, 0x1AU);
static const struct editor_rgb stage_fill = EDITOR_RGB(0x0EU, 0x0EU, 0x0EU);
static const struct editor_rgb lane_fill = EDITOR_RGB(0x1EU, 0x1EU, 0x1EU);
static const struct editor_rgb rule = EDITOR_RGB(0x33U, 0x33U, 0x33U);
static const struct editor_rgb ink = EDITOR_RGB(0xF0U, 0xF0U, 0xF0U);
static const struct editor_rgb ink_soft = EDITOR_RGB(0xAAU, 0xAAU, 0xAAU);
static const struct editor_rgb ink_faint = EDITOR_RGB(0x77U, 0x77U, 0x77U);
static const struct editor_rgb accent = EDITOR_RGB(0x00U, 0x78U, 0xD4U);
static const struct editor_rgb accent_soft = EDITOR_RGB(0x0AU, 0x50U, 0x86U);
static const struct editor_rgb hover_fill = EDITOR_RGB(0x32U, 0x32U, 0x32U);
static const struct editor_rgb close_hot = EDITOR_RGB(0xE8U, 0x11U, 0x23U);
static const struct editor_rgb clip_fill = EDITOR_RGB(0x2FU, 0x4AU, 0x63U);
static const struct editor_rgb text_fill = EDITOR_RGB(0x3EU, 0x5AU, 0x36U);
static const struct editor_rgb effect_fill = EDITOR_RGB(0x5AU, 0x3EU, 0x62U);
static const struct editor_rgb playhead = EDITOR_RGB(0xFFU, 0xFFU, 0xFFU);

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool window_open;

static struct editor_clip clip;
static const uint32_t *poster_pixels;
static uint32_t poster_width;
static uint32_t poster_height;

static struct editor_item items[EDITOR_MAX_ITEMS];
static size_t selected = (size_t)-1;
static enum editor_track library_tab = EDITOR_TRACK_TEXT;

static uint32_t playhead_ms;
static bool playing;
static uint64_t played_from_ns;
static uint32_t played_from_ms;

static size_t hover_preset = (size_t)-1;
static size_t hover_item = (size_t)-1;
static size_t hover_tab = (size_t)-1;
static size_t hover_field = (size_t)-1;
static bool hover_transport;
static bool hover_close;

static const char *self_test_failure = "";

const char *editor_status_string(enum editor_status status)
{
    switch (status) {
    case EDITOR_STATUS_OK:
        return "ok";
    case EDITOR_STATUS_NULL_ARGUMENT:
        return "null argument";
    case EDITOR_STATUS_NOT_INITIALIZED:
        return "editor not initialized";
    case EDITOR_STATUS_BAD_INDEX:
        return "editor index is out of range";
    case EDITOR_STATUS_FULL:
        return "the editor's timeline is full";
    case EDITOR_STATUS_UNSUPPORTED_GEOMETRY:
        return "editor geometry is unsupported";
    case EDITOR_STATUS_SURFACE_FAILURE:
        return "editor surface refused a pixel";
    default:
        return "unknown editor status";
    }
}

/* ============================================================== PRIMITIVES */

static uint32_t pack_rgb(struct editor_rgb colour)
{
    return framebuffer_pack(colour.red, colour.green, colour.blue);
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
    return point.x >= 0 && point.y >= 0 &&
        (uint32_t)point.x >= area.x &&
        (uint32_t)point.x < area.x + area.width &&
        (uint32_t)point.y >= area.y &&
        (uint32_t)point.y < area.y + area.height;
}

static bool names_match(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static enum editor_status fill(struct ui_rect area, struct ui_rect damage,
    struct editor_rgb colour)
{
    const struct ui_rect clipped = intersect(intersect(area, damage),
        window_rect);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return EDITOR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return EDITOR_STATUS_OK;
}

static enum editor_status outline(struct ui_rect box, struct ui_rect damage,
    struct editor_rgb colour)
{
    enum editor_status status;

    if (box.width == 0U || box.height == 0U) {
        return EDITOR_STATUS_OK;
    }
    status = fill((struct ui_rect){ box.x, box.y, box.width, 1U }, damage,
        colour);
    if (status == EDITOR_STATUS_OK) {
        status = fill((struct ui_rect){ box.x, box.y + box.height - 1U,
            box.width, 1U }, damage, colour);
    }
    if (status == EDITOR_STATUS_OK) {
        status = fill((struct ui_rect){ box.x, box.y, 1U, box.height },
            damage, colour);
    }
    if (status == EDITOR_STATUS_OK) {
        status = fill((struct ui_rect){ box.x + box.width - 1U, box.y, 1U,
            box.height }, damage, colour);
    }
    return status;
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

static enum editor_status text_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct editor_rgb colour)
{
    const struct ui_rect visible = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (visible.width == 0U || visible.height == 0U || body == NULL ||
            body[0] == '\0') {
        return EDITOR_STATUS_OK;
    }
    bounds.x = window_rect.x;
    bounds.y = window_rect.y;
    bounds.width = window_rect.width;
    bounds.height = window_rect.height;
    region.x = visible.x;
    region.y = visible.y;
    region.width = visible.width;
    region.height = visible.height;
    (void)ui_font_draw_text_clipped(canvas, bounds, region, x, baseline, body,
        pack_rgb(colour), NULL);
    return EDITOR_STATUS_OK;
}

/* Text that stops at a limit rather than running into what is beside it. */
static enum editor_status text_clipped(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct editor_rgb colour,
    uint32_t limit)
{
    return text_at(intersect(damage, (struct ui_rect){ x, window_rect.y,
        limit, window_rect.height }), x, baseline, body, colour);
}

static const uint8_t *glyph_cell(const char *name, uint32_t wanted,
    uint32_t *size)
{
    for (size_t index = 0U; index < EDITOR_LUCIDE_COUNT; ++index) {
        size_t choice = 0U;

        if (!names_match(editor_lucide[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < EDITOR_LUCIDE_SIZES; ++option) {
            if (editor_lucide_size[option] <= wanted) {
                choice = option;
            }
        }
        *size = editor_lucide_size[choice];
        return editor_lucide[index].alpha[choice];
    }
    return NULL;
}

static enum editor_status draw_glyph(const char *name, struct ui_rect box,
    struct ui_rect damage, struct editor_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t wanted = box.width < box.height ? box.width : box.height;
    const uint32_t over = pack_rgb(colour);
    uint32_t size = 0U;
    const uint8_t *cell = glyph_cell(name, wanted, &size);
    struct ui_rect placed;
    struct ui_rect clipped;

    if (cell == NULL || size == 0U) {
        return EDITOR_STATUS_OK;
    }
    placed = (struct ui_rect){
        box.x + (box.width > size ? (box.width - size) / 2U : 0U),
        box.y + (box.height > size ? (box.height - size) / 2U : 0U),
        size, size };
    clipped = intersect(intersect(placed, damage), window_rect);
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
                return EDITOR_STATUS_SURFACE_FAILURE;
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
                return EDITOR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return EDITOR_STATUS_OK;
}

/* ================================================================ NUMBERS */

static size_t append_literal(char *out, size_t bytes, size_t at,
    const char *body)
{
    while (body != NULL && *body != '\0' && at + 1U < bytes) {
        out[at] = *body;
        ++at;
        ++body;
    }
    if (at < bytes) {
        out[at] = '\0';
    }
    return at;
}

static size_t append_uint(char *out, size_t bytes, size_t at, uint32_t value,
    uint32_t least_digits)
{
    char digits[12];
    size_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        ++count;
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count < least_digits && count < sizeof(digits)) {
        digits[count] = '0';
        ++count;
    }
    while (count > 0U && at + 1U < bytes) {
        --count;
        out[at] = digits[count];
        ++at;
    }
    if (at < bytes) {
        out[at] = '\0';
    }
    return at;
}

/* A position as an editor writes one: seconds and hundredths. */
static void write_time(char *out, size_t bytes, uint32_t position_ms)
{
    size_t at = append_uint(out, bytes, 0U, position_ms / 1000U, 1U);

    at = append_literal(out, bytes, at, ".");
    (void)append_uint(out, bytes, at, (position_ms % 1000U) / 10U, 2U);
}

/* ================================================================ LIBRARY
 *
 * The presets, which are the module's own because the module is what
 * performs them.  A caller supplies the clip and the items on the timeline;
 * what a text style or an effect DOES lives here, so the list of them does
 * too.
 */

static const char *const style_name[EDITOR_STYLE_COUNT] = {
    "Caption", "Body", "Title"
};

static const char *const effect_name[EDITOR_EFFECT_COUNT] = {
    "Fade", "Mono", "Warm", "Vignette"
};

/* What a text preset drops on the timeline when it is pressed. */
static const char *const style_sample[EDITOR_STYLE_COUNT] = {
    "Subtitle goes here", "Your text", "TITLE"
};

static size_t preset_count(void)
{
    return library_tab == EDITOR_TRACK_TEXT ? (size_t)EDITOR_STYLE_COUNT :
        (size_t)EDITOR_EFFECT_COUNT;
}

static const char *preset_name(size_t index)
{
    if (library_tab == EDITOR_TRACK_TEXT) {
        return index < EDITOR_STYLE_COUNT ? style_name[index] : "";
    }
    return index < EDITOR_EFFECT_COUNT ? effect_name[index] : "";
}

/* =============================================================== GEOMETRY */

struct ui_rect editor_bounds(void)
{
    return window_rect;
}

static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){ window_rect.x + EDITOR_BORDER,
        window_rect.y + EDITOR_BORDER,
        window_rect.width - EDITOR_BORDER * 2U, EDITOR_CAPTION };
}

static struct ui_rect caption_button_rect(size_t index)
{
    const struct ui_rect bar = caption_rect();

    if (index >= 3U || bar.width < EDITOR_CAPTION_BUTTON * 3U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ bar.x + bar.width -
        EDITOR_CAPTION_BUTTON * (3U - (uint32_t)index), bar.y,
        EDITOR_CAPTION_BUTTON, bar.height };
}

/* Everything under the caption and above the timeline. */
static struct ui_rect middle_rect(void)
{
    const struct ui_rect bar = caption_rect();

    return (struct ui_rect){ bar.x, bar.y + bar.height, bar.width,
        window_rect.height - EDITOR_BORDER * 2U - EDITOR_CAPTION -
            EDITOR_TIMELINE };
}

static struct ui_rect library_rect(void)
{
    const struct ui_rect middle = middle_rect();

    return (struct ui_rect){ middle.x, middle.y, EDITOR_LIBRARY,
        middle.height };
}

static struct ui_rect library_tab_rect(size_t index)
{
    const struct ui_rect area = library_rect();
    const uint32_t width = area.width / 2U;

    if (index >= 2U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ area.x + (uint32_t)index * width, area.y,
        index == 1U ? area.width - width : width, EDITOR_TAB_HEIGHT };
}

static struct ui_rect preset_rect(size_t index)
{
    const struct ui_rect area = library_rect();
    const uint32_t top = area.y + EDITOR_TAB_HEIGHT + EDITOR_PRESET_PAD +
        (uint32_t)index * EDITOR_PRESET_HEIGHT;

    if (index >= preset_count() ||
            top + EDITOR_PRESET_HEIGHT > area.y + area.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ area.x + EDITOR_PRESET_PAD, top,
        area.width - EDITOR_PRESET_PAD * 2U, EDITOR_PRESET_HEIGHT - 6U };
}

static struct ui_rect properties_rect(void)
{
    const struct ui_rect middle = middle_rect();

    return (struct ui_rect){ middle.x + middle.width - EDITOR_PROPERTIES,
        middle.y, EDITOR_PROPERTIES, middle.height };
}

static struct ui_rect player_rect(void)
{
    const struct ui_rect middle = middle_rect();

    return (struct ui_rect){ middle.x + EDITOR_LIBRARY, middle.y,
        middle.width - EDITOR_LIBRARY - EDITOR_PROPERTIES, middle.height };
}

struct ui_rect editor_stage_rect(void)
{
    const struct ui_rect player = player_rect();

    return (struct ui_rect){ player.x, player.y, player.width,
        player.height - EDITOR_TRANSPORT };
}

static struct ui_rect transport_rect(void)
{
    const struct ui_rect player = player_rect();

    return (struct ui_rect){ player.x, player.y + player.height -
        EDITOR_TRANSPORT, player.width, EDITOR_TRANSPORT };
}

static struct ui_rect transport_button_rect(void)
{
    const struct ui_rect bar = transport_rect();

    return (struct ui_rect){ bar.x + 10U,
        bar.y + (bar.height - EDITOR_TRANSPORT_BUTTON) / 2U,
        EDITOR_TRANSPORT_BUTTON, EDITOR_TRANSPORT_BUTTON };
}

/* The scrub bar, which is the player's own view of the same time the
 * timeline shows - dragging either moves the other. */
static struct ui_rect scrub_rect(void)
{
    const struct ui_rect bar = transport_rect();
    const uint32_t left = bar.x + 10U + EDITOR_TRANSPORT_BUTTON + 12U;
    const uint32_t right = bar.x + bar.width - 74U;

    if (right <= left) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ left, bar.y + bar.height / 2U -
        EDITOR_SCRUB_TRACK / 2U, right - left, EDITOR_SCRUB_TRACK };
}

static struct ui_rect timeline_rect(void)
{
    return (struct ui_rect){ window_rect.x + EDITOR_BORDER,
        window_rect.y + window_rect.height - EDITOR_BORDER - EDITOR_TIMELINE,
        window_rect.width - EDITOR_BORDER * 2U, EDITOR_TIMELINE };
}

static struct ui_rect ruler_rect(void)
{
    const struct ui_rect area = timeline_rect();

    return (struct ui_rect){ area.x + EDITOR_GUTTER, area.y +
        EDITOR_TIMELINE_PAD, area.width - EDITOR_GUTTER -
        EDITOR_TIMELINE_PAD, EDITOR_RULER };
}

static struct ui_rect lane_rect(size_t track)
{
    const struct ui_rect ruler = ruler_rect();

    if (track >= EDITOR_TRACK_COUNT) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ ruler.x, ruler.y + ruler.height +
        (uint32_t)track * EDITOR_LANE_HEIGHT, ruler.width,
        EDITOR_LANE_HEIGHT - 6U };
}

static uint32_t clip_length(void)
{
    return clip.length_ms == 0U ? 1U : clip.length_ms;
}

/* Time to a column, and back.  The lane spans the whole clip: this editor
 * does not zoom, because a zoom control that cannot be dragged is a picture
 * of one. */
static uint32_t time_to_x(struct ui_rect lane, uint32_t position_ms)
{
    const uint32_t length = clip_length();
    const uint32_t clamped = position_ms > length ? length : position_ms;

    return lane.x + (uint32_t)(((uint64_t)clamped * lane.width) / length);
}

static uint32_t x_to_time(struct ui_rect lane, int32_t x)
{
    if (lane.width == 0U || x <= (int32_t)lane.x) {
        return 0U;
    }
    if ((uint32_t)x >= lane.x + lane.width) {
        return clip_length();
    }
    return (uint32_t)(((uint64_t)((uint32_t)x - lane.x) * clip_length()) /
        lane.width);
}

static struct ui_rect item_rect(size_t index)
{
    const struct editor_item *entry;
    struct ui_rect lane;
    uint32_t left;
    uint32_t right;

    if (index >= EDITOR_MAX_ITEMS || !items[index].present) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    entry = &items[index];
    lane = lane_rect((size_t)entry->track);
    if (lane.width == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    left = time_to_x(lane, entry->start_ms);
    right = time_to_x(lane, entry->start_ms + entry->length_ms);
    if (right <= left + 2U) {
        right = left + 2U;
    }
    if (right > lane.x + lane.width) {
        right = lane.x + lane.width;
    }
    return (struct ui_rect){ left, lane.y, right - left, lane.height };
}

/* The properties panel's rows, which depend on what is selected. */
static size_t field_count(void)
{
    if (selected == (size_t)-1 || !items[selected].present) {
        return 0U;
    }
    /* Kind, strength, delete - and a text item also picks a style. */
    return items[selected].track == EDITOR_TRACK_TEXT ? 4U : 3U;
}

static struct ui_rect field_rect(size_t index)
{
    const struct ui_rect area = properties_rect();
    const uint32_t top = area.y + 56U + (uint32_t)index * EDITOR_ROW_HEIGHT;

    if (index >= field_count() ||
            top + EDITOR_ROW_HEIGHT > area.y + area.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ area.x + 14U, top, area.width - 28U,
        EDITOR_ROW_HEIGHT };
}

/* The control inside a row, which is what a press has to land on. */
static struct ui_rect field_control_rect(size_t index)
{
    const struct ui_rect row = field_rect(index);

    if (row.width == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ row.x, row.y + row.height -
        EDITOR_FIELD_HEIGHT - 4U, row.width, EDITOR_FIELD_HEIGHT };
}

/* ================================================================ EFFECTS
 *
 * Each is a function of one pixel and where that pixel sits in the stage.
 * No frame buffer of its own, no second pass, nothing that needs to see the
 * picture twice - which is why four of them can be laid over a poster and
 * composited in one walk.
 */

struct editor_mix {
    uint32_t red;
    uint32_t green;
    uint32_t blue;
};

static struct editor_mix apply_effect(struct editor_mix colour,
    enum editor_effect effect, uint32_t weight, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
    uint32_t target_red = colour.red;
    uint32_t target_green = colour.green;
    uint32_t target_blue = colour.blue;

    if (weight == 0U) {
        return colour;
    }
    switch (effect) {
    case EDITOR_EFFECT_FADE:
        target_red = 0U;
        target_green = 0U;
        target_blue = 0U;
        break;
    case EDITOR_EFFECT_MONO: {
        /* Rec. 601 luma, in integers: the weights the rest of this shell
         * uses when it needs a grey out of a colour. */
        const uint32_t grey = (77U * colour.red + 150U * colour.green +
            29U * colour.blue) / 256U;

        target_red = grey;
        target_green = grey;
        target_blue = grey;
        break;
    }
    case EDITOR_EFFECT_WARM:
        target_red = colour.red + (255U - colour.red) / 3U;
        target_green = colour.green + (255U - colour.green) / 8U;
        target_blue = colour.blue * 3U / 4U;
        break;
    case EDITOR_EFFECT_VIGNETTE:
    default: {
        /*
         * Distance from the centre, as a fraction of the half diagonal,
         * squared - which is the falloff a vignette has.  All integer: the
         * kernel builds with -msoft-float and a float here would not link.
         */
        const int32_t dx = (int32_t)x * 2 - (int32_t)width;
        const int32_t dy = (int32_t)y * 2 - (int32_t)height;
        const uint64_t span = (uint64_t)((int64_t)dx * dx) +
            (uint64_t)((int64_t)dy * dy);
        const uint64_t reach = (uint64_t)width * width +
            (uint64_t)height * height;
        uint32_t fall = reach == 0U ? 0U :
            (uint32_t)((span * 255U) / reach);

        if (fall > 255U) {
            fall = 255U;
        }
        target_red = colour.red * (255U - fall) / 255U;
        target_green = colour.green * (255U - fall) / 255U;
        target_blue = colour.blue * (255U - fall) / 255U;
        break;
    }
    }
    if (weight >= 255U) {
        return (struct editor_mix){ target_red, target_green, target_blue };
    }
    return (struct editor_mix){
        (colour.red * (255U - weight) + target_red * weight) / 255U,
        (colour.green * (255U - weight) + target_green * weight) / 255U,
        (colour.blue * (255U - weight) + target_blue * weight) / 255U };
}

/* Whether an item covers the playhead, and how hard it is laid on there. */
static bool item_active(const struct editor_item *entry, uint32_t *weight)
{
    if (!entry->present || playhead_ms < entry->start_ms ||
            playhead_ms >= entry->start_ms + entry->length_ms) {
        return false;
    }
    *weight = (uint32_t)entry->strength * 255U / 100U;
    if (entry->track == EDITOR_TRACK_EFFECT &&
            entry->effect == EDITOR_EFFECT_FADE && entry->length_ms != 0U) {
        /* A fade is the one effect whose weight moves WITHIN the item: it
         * is a fade, so it has to arrive somewhere. */
        *weight = *weight * (playhead_ms - entry->start_ms) /
            entry->length_ms;
    }
    return true;
}

/* ================================================================ DRAWING */

static enum editor_status draw_caption(struct ui_rect damage)
{
    const struct ui_rect bar = caption_rect();
    enum editor_status status = fill(bar, damage, caption_fill);

    if (status == EDITOR_STATUS_OK) {
        status = draw_glyph("video", (struct ui_rect){ bar.x + 8U, bar.y,
            16U, bar.height }, damage, ink_soft);
    }
    if (status == EDITOR_STATUS_OK) {
        status = text_at(damage, bar.x + 32U, bar.y + bar.height / 2U + 5U,
            "Media Editor", ink);
    }
    for (size_t index = 0U; index < 3U && status == EDITOR_STATUS_OK;
         ++index) {
        const struct ui_rect button = caption_button_rect(index);
        const uint32_t mid_x = button.x + button.width / 2U;
        const uint32_t mid_y = button.y + button.height / 2U;
        const uint32_t half = EDITOR_CAPTION_MARK / 2U;
        struct editor_rgb mark = ink_soft;

        if (button.width == 0U) {
            continue;
        }
        if (index == 2U && hover_close) {
            status = fill(button, damage, close_hot);
            mark = ink;
        }
        if (status != EDITOR_STATUS_OK) {
            return status;
        }
        if (index == 0U) {
            status = fill((struct ui_rect){ mid_x - half, mid_y,
                EDITOR_CAPTION_MARK, 1U }, damage, mark);
        } else if (index == 1U) {
            status = outline((struct ui_rect){ mid_x - half, mid_y - half,
                EDITOR_CAPTION_MARK, EDITOR_CAPTION_MARK }, damage, mark);
        } else {
            for (uint32_t step = 0U; step < EDITOR_CAPTION_MARK &&
                    status == EDITOR_STATUS_OK; ++step) {
                status = fill((struct ui_rect){ mid_x - half + step,
                    mid_y - half + step, 1U, 1U }, damage, mark);
                if (status == EDITOR_STATUS_OK) {
                    status = fill((struct ui_rect){ mid_x - half + step,
                        mid_y + half - 1U - step, 1U, 1U }, damage, mark);
                }
            }
        }
    }
    return status;
}

static enum editor_status draw_library(struct ui_rect damage)
{
    const struct ui_rect area = library_rect();
    enum editor_status status = fill(area, damage, panel);

    if (status == EDITOR_STATUS_OK) {
        status = fill((struct ui_rect){ area.x + area.width - 1U, area.y, 1U,
            area.height }, damage, rule);
    }
    /* The tab strip: two, because there are two libraries. */
    for (size_t index = 0U; index < 2U && status == EDITOR_STATUS_OK;
         ++index) {
        const struct ui_rect tab = library_tab_rect(index);
        const bool chosen = (index == 0U) == (library_tab ==
            EDITOR_TRACK_TEXT);
        const char *mark = index == 0U ? "type" : "sparkle";
        const char *label = index == 0U ? "Text" : "Effects";
        const uint32_t width = width_of(label);
        const uint32_t glyph_left = tab.x + (tab.width - EDITOR_TAB_GLYPH -
            6U - width) / 2U;

        if (!chosen && index == hover_tab) {
            status = fill(tab, damage, hover_fill);
        } else if (chosen) {
            status = fill(tab, damage, panel_deep);
        }
        if (status == EDITOR_STATUS_OK) {
            status = draw_glyph(mark, (struct ui_rect){ glyph_left, tab.y,
                EDITOR_TAB_GLYPH, tab.height }, damage,
                chosen ? accent : ink_soft);
        }
        if (status == EDITOR_STATUS_OK) {
            status = text_at(damage, glyph_left + EDITOR_TAB_GLYPH + 6U,
                tab.y + tab.height / 2U + 5U, label, chosen ? ink :
                ink_soft);
        }
        /* The chosen tab carries an accent underline, which is where
         * Windows 10 puts the mark of a selected tab. */
        if (status == EDITOR_STATUS_OK && chosen) {
            status = fill((struct ui_rect){ tab.x, tab.y + tab.height - 2U,
                tab.width, 2U }, damage, accent);
        }
    }
    if (status == EDITOR_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y + EDITOR_TAB_HEIGHT,
            area.width, 1U }, damage, rule);
    }
    for (size_t index = 0U; index < preset_count() &&
            status == EDITOR_STATUS_OK; ++index) {
        const struct ui_rect box = preset_rect(index);
        const bool hot = index == hover_preset;

        if (box.width == 0U) {
            continue;
        }
        status = fill(box, damage, hot ? hover_fill : panel_deep);
        if (status == EDITOR_STATUS_OK) {
            status = outline(box, damage, hot ? accent : rule);
        }
        if (status == EDITOR_STATUS_OK) {
            status = text_clipped(damage, box.x + 12U,
                box.y + box.height / 2U + 5U, preset_name(index), ink,
                box.width - 42U);
        }
        /* Every preset says how it is used, because it is a button that
         * adds something rather than one that turns something on. */
        if (status == EDITOR_STATUS_OK) {
            status = draw_glyph("plus", (struct ui_rect){
                box.x + box.width - 28U, box.y, 16U, box.height }, damage,
                hot ? accent : ink_faint);
        }
    }
    return status;
}

/*
 * The player.  The poster is composited one to one, every effect covering
 * the playhead is applied as the pixel is written, and the text items
 * covering it are drawn over the result.
 */
static enum editor_status draw_stage(struct ui_rect damage)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const struct ui_rect stage = editor_stage_rect();
    struct ui_rect placed;
    struct ui_rect clipped;
    enum editor_status status = fill(stage, damage, stage_fill);

    if (status != EDITOR_STATUS_OK) {
        return status;
    }
    if (poster_pixels == NULL || poster_width == 0U || poster_height == 0U) {
        static const char empty[] = "No clip open";

        return text_at(damage,
            stage.x + (stage.width - width_of(empty)) / 2U,
            stage.y + stage.height / 2U, empty, ink_faint);
    }
    placed = (struct ui_rect){
        stage.x + (stage.width > poster_width ?
            (stage.width - poster_width) / 2U : 0U),
        stage.y + (stage.height > poster_height ?
            (stage.height - poster_height) / 2U : 0U),
        poster_width, poster_height };
    clipped = intersect(intersect(placed, damage), window_rect);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - placed.x + x;
            const uint32_t stored = poster_pixels[(size_t)local_y *
                poster_width + local_x];
            struct editor_mix colour = { (stored >> 16) & 0xFFU,
                (stored >> 8) & 0xFFU, stored & 0xFFU };

            for (size_t index = 0U; index < EDITOR_MAX_ITEMS; ++index) {
                uint32_t weight = 0U;

                if (items[index].track != EDITOR_TRACK_EFFECT ||
                        !item_active(&items[index], &weight)) {
                    continue;
                }
                colour = apply_effect(colour, items[index].effect, weight,
                    local_x, local_y, poster_width, poster_height);
            }
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (colour.red << format.red_position) |
                    (colour.green << format.green_position) |
                    (colour.blue << format.blue_position)) !=
                    SURFACE_STATUS_OK) {
                return EDITOR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    /* The words, over the picture. */
    for (size_t index = 0U; index < EDITOR_MAX_ITEMS &&
            status == EDITOR_STATUS_OK; ++index) {
        const struct editor_item *entry = &items[index];
        uint32_t weight = 0U;
        uint32_t baseline;
        uint32_t width;

        if (entry->track != EDITOR_TRACK_TEXT ||
                !item_active(entry, &weight) || weight < 16U) {
            continue;
        }
        width = width_of(entry->label);
        switch (entry->style) {
        case EDITOR_STYLE_TITLE:
            baseline = placed.y + placed.height / 3U;
            break;
        case EDITOR_STYLE_BODY:
            baseline = placed.y + placed.height / 2U;
            break;
        case EDITOR_STYLE_CAPTION:
        default:
            baseline = placed.y + placed.height - 34U;
            break;
        }
        /*
         * A shadow plate under the words.  Type laid straight onto a
         * photograph is unreadable wherever the photograph is pale, which
         * is why every captioning tool draws one.
         */
        status = fill((struct ui_rect){
            placed.x + (placed.width - width) / 2U - 10U, baseline - 17U,
            width + 20U, 24U }, intersect(damage, placed), stage_fill);
        if (status == EDITOR_STATUS_OK) {
            status = text_at(intersect(damage, placed),
                placed.x + (placed.width - width) / 2U, baseline,
                entry->label, ink);
        }
    }
    return status;
}

static enum editor_status draw_transport(struct ui_rect damage)
{
    const struct ui_rect bar = transport_rect();
    const struct ui_rect button = transport_button_rect();
    const struct ui_rect scrub = scrub_rect();
    char reading[24];
    enum editor_status status = fill(bar, damage, panel);

    if (status == EDITOR_STATUS_OK) {
        status = fill((struct ui_rect){ bar.x, bar.y, bar.width, 1U },
            damage, rule);
    }
    if (status == EDITOR_STATUS_OK && hover_transport) {
        status = fill(button, damage, hover_fill);
    }
    if (status == EDITOR_STATUS_OK) {
        status = draw_glyph(playing ? "pause" : "play", (struct ui_rect){
            button.x, button.y, button.width, button.height }, damage,
            hover_transport ? accent : ink);
    }
    if (status == EDITOR_STATUS_OK && scrub.width != 0U) {
        const uint32_t head = time_to_x(scrub, playhead_ms);

        status = fill(scrub, damage, rule);
        if (status == EDITOR_STATUS_OK) {
            status = fill((struct ui_rect){ scrub.x, scrub.y,
                head - scrub.x, scrub.height }, damage, accent);
        }
        if (status == EDITOR_STATUS_OK) {
            status = fill((struct ui_rect){
                head - EDITOR_SLIDER_THUMB / 2U,
                scrub.y + scrub.height / 2U - EDITOR_SLIDER_THUMB / 2U,
                EDITOR_SLIDER_THUMB, EDITOR_SLIDER_THUMB }, damage, ink);
        }
    }
    /* The reading, which is the playhead over the clip's length. */
    write_time(reading, sizeof(reading), playhead_ms);
    {
        char total[24];
        char line[52];
        size_t cursor;

        write_time(total, sizeof(total), clip.length_ms);
        cursor = append_literal(line, sizeof(line), 0U, reading);
        cursor = append_literal(line, sizeof(line), cursor, " / ");
        (void)append_literal(line, sizeof(line), cursor, total);
        if (status == EDITOR_STATUS_OK) {
            status = text_at(damage, bar.x + bar.width - width_of(line) -
                12U, bar.y + bar.height / 2U + 5U, line, ink_soft);
        }
    }
    return status;
}

static enum editor_status draw_properties(struct ui_rect damage)
{
    const struct ui_rect area = properties_rect();
    const struct editor_item *entry = selected != (size_t)-1 &&
        items[selected].present ? &items[selected] : NULL;
    enum editor_status status = fill(area, damage, panel);

    if (status == EDITOR_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y, 1U, area.height },
            damage, rule);
    }
    if (status == EDITOR_STATUS_OK) {
        status = text_at(damage, area.x + 14U, area.y + 26U,
            entry == NULL ? "Nothing selected" : "Selected", ink);
    }
    if (entry == NULL) {
        if (status == EDITOR_STATUS_OK) {
            status = text_clipped(damage, area.x + 14U, area.y + 48U,
                "Pick something on the timeline.", ink_faint,
                area.width - 28U);
        }
        return status;
    }
    for (size_t index = 0U; index < field_count() &&
            status == EDITOR_STATUS_OK; ++index) {
        const struct ui_rect row = field_rect(index);
        const struct ui_rect control = field_control_rect(index);
        const bool hot = index == hover_field;
        const char *label;
        char reading[24];

        if (row.width == 0U) {
            continue;
        }
        if (index == 0U) {
            label = "Kind";
        } else if (index == 1U && entry->track == EDITOR_TRACK_TEXT) {
            label = "Style";
        } else if (field_count() == 4U ? index == 2U : index == 1U) {
            label = "Strength";
        } else {
            label = "";
        }
        if (label[0] != '\0') {
            status = text_at(damage, row.x, row.y + 14U, label, ink_faint);
        }
        if (status != EDITOR_STATUS_OK) {
            continue;
        }
        if (index == 0U) {
            /* What this is, which is a reading and not a control - the one
             * row here that is not pressable, and it is not drawn as a
             * box so it does not claim to be. */
            status = text_at(damage, control.x,
                control.y + control.height / 2U + 5U,
                entry->track == EDITOR_TRACK_TEXT ? "Text" :
                    effect_name[entry->effect], ink);
        } else if (index == 1U && entry->track == EDITOR_TRACK_TEXT) {
            status = fill(control, damage, hot ? hover_fill : panel_deep);
            if (status == EDITOR_STATUS_OK) {
                status = outline(control, damage, hot ? accent : rule);
            }
            if (status == EDITOR_STATUS_OK) {
                status = text_at(damage, control.x + 8U,
                    control.y + control.height / 2U + 5U,
                    style_name[entry->style], ink);
            }
            if (status == EDITOR_STATUS_OK) {
                status = draw_glyph("chevron-down", (struct ui_rect){
                    control.x + control.width - 22U, control.y, 16U,
                    control.height }, damage, ink_soft);
            }
        } else if (field_count() == 4U ? index == 2U : index == 1U) {
            const uint32_t track_y = control.y + control.height / 2U -
                EDITOR_SLIDER_TRACK / 2U;
            const uint32_t span = control.width - EDITOR_SLIDER_THUMB - 40U;
            const uint32_t lit = span * entry->strength / 100U;

            status = fill((struct ui_rect){ control.x, track_y, span,
                EDITOR_SLIDER_TRACK }, damage, rule);
            if (status == EDITOR_STATUS_OK) {
                status = fill((struct ui_rect){ control.x, track_y, lit,
                    EDITOR_SLIDER_TRACK }, damage, hot ? accent :
                    accent_soft);
            }
            if (status == EDITOR_STATUS_OK) {
                status = fill((struct ui_rect){ control.x + lit,
                    control.y + control.height / 2U -
                        EDITOR_SLIDER_THUMB / 2U,
                    EDITOR_SLIDER_THUMB, EDITOR_SLIDER_THUMB }, damage,
                    hot ? accent : ink);
            }
            if (status == EDITOR_STATUS_OK) {
                size_t cursor = append_uint(reading, sizeof(reading), 0U,
                    entry->strength, 1U);

                (void)append_literal(reading, sizeof(reading), cursor, "%");
                status = text_at(damage, control.x + span + 14U,
                    control.y + control.height / 2U + 5U, reading, ink_soft);
            }
        } else {
            static const char remove[] = "Remove";

            status = fill(control, damage, hot ? hover_fill : panel_deep);
            if (status == EDITOR_STATUS_OK) {
                status = outline(control, damage, hot ? accent : rule);
            }
            if (status == EDITOR_STATUS_OK) {
                status = draw_glyph("trash-2", (struct ui_rect){
                    control.x + 8U, control.y, 16U, control.height }, damage,
                    hot ? accent : ink_soft);
            }
            if (status == EDITOR_STATUS_OK) {
                status = text_at(damage, control.x + 30U,
                    control.y + control.height / 2U + 5U, remove, ink);
            }
        }
    }
    return status;
}

static enum editor_status draw_timeline(struct ui_rect damage)
{
    static const char *const lane_label[EDITOR_TRACK_COUNT] = {
        "Clip", "Text", "Effect"
    };
    const struct ui_rect area = timeline_rect();
    const struct ui_rect ruler = ruler_rect();
    enum editor_status status = fill(area, damage, panel_deep);

    if (status == EDITOR_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y, area.width, 1U },
            damage, rule);
    }
    /* The ruler: a mark every second, a longer one and a reading every
     * five, which is the density a timeline this wide can carry. */
    for (uint32_t second = 0U;
            second * 1000U <= clip_length() && status == EDITOR_STATUS_OK;
            ++second) {
        const uint32_t x = time_to_x(ruler, second * 1000U);
        const bool major = (second % 5U) == 0U;

        status = fill((struct ui_rect){ x, ruler.y + ruler.height -
            (major ? 10U : 5U), 1U, major ? 10U : 5U }, damage,
            major ? ink_faint : rule);
        if (status == EDITOR_STATUS_OK && major) {
            char reading[16];
            size_t cursor = append_uint(reading, sizeof(reading), 0U,
                second, 1U);

            (void)append_literal(reading, sizeof(reading), cursor, "s");
            status = text_at(damage, x + 3U, ruler.y + 10U, reading,
                ink_faint);
        }
    }
    for (size_t track = 0U; track < EDITOR_TRACK_COUNT &&
            status == EDITOR_STATUS_OK; ++track) {
        const struct ui_rect lane = lane_rect(track);

        status = fill(lane, damage, lane_fill);
        if (status == EDITOR_STATUS_OK) {
            status = text_at(damage, area.x + EDITOR_TIMELINE_PAD,
                lane.y + lane.height / 2U + 5U, lane_label[track],
                ink_faint);
        }
    }
    /*
     * The clip itself, filling its lane end to end.
     *
     * It is drawn FLAT - no border, no hover, no selection outline - and
     * nothing here hit-tests it, because this editor cannot trim, split or
     * move the footage it was handed and a chip that looks pressable had
     * better be.  What it is is the thing the other two lanes are laid over,
     * so it is drawn as a bed rather than as a control.
     */
    if (status == EDITOR_STATUS_OK) {
        const struct ui_rect lane = lane_rect(EDITOR_TRACK_CLIP);

        status = fill(lane, damage, clip_fill);
        if (status == EDITOR_STATUS_OK && clip.name[0] != '\0') {
            status = text_clipped(damage, lane.x + 8U,
                lane.y + lane.height / 2U + 5U, clip.name, ink_soft,
                lane.width - 16U);
        }
    }
    for (size_t index = 0U; index < EDITOR_MAX_ITEMS &&
            status == EDITOR_STATUS_OK; ++index) {
        const struct editor_item *entry = &items[index];
        const struct ui_rect box = item_rect(index);
        struct editor_rgb body;

        if (!entry->present || box.width == 0U) {
            continue;
        }
        body = entry->track == EDITOR_TRACK_CLIP ? clip_fill :
            (entry->track == EDITOR_TRACK_TEXT ? text_fill : effect_fill);
        status = fill(box, damage, body);
        if (status == EDITOR_STATUS_OK) {
            status = outline(box, damage, index == selected ? accent :
                (index == hover_item ? ink_soft : rule));
        }
        if (status == EDITOR_STATUS_OK && box.width > 22U) {
            status = text_clipped(damage, box.x + 7U,
                box.y + box.height / 2U + 5U,
                entry->track == EDITOR_TRACK_EFFECT ?
                    effect_name[entry->effect] : entry->label, ink,
                box.width - 12U);
        }
    }
    /* The playhead, over everything, with a grip on the ruler. */
    if (status == EDITOR_STATUS_OK) {
        const uint32_t x = time_to_x(ruler, playhead_ms);
        const struct ui_rect last = lane_rect(EDITOR_TRACK_COUNT - 1U);

        status = fill((struct ui_rect){ x, ruler.y, 1U,
            last.y + last.height - ruler.y }, damage, playhead);
        if (status == EDITOR_STATUS_OK) {
            status = fill((struct ui_rect){ x - EDITOR_PLAYHEAD_GRIP / 2U,
                ruler.y, EDITOR_PLAYHEAD_GRIP, 6U }, damage, playhead);
        }
    }
    return status;
}

enum editor_status editor_draw(struct ui_rect damage)
{
    const struct ui_rect clipped = intersect(damage, window_rect);
    enum editor_status status;

    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open || clipped.width == 0U || clipped.height == 0U) {
        return EDITOR_STATUS_OK;
    }
    status = fill(window_rect, clipped, accent);
    if (status == EDITOR_STATUS_OK) {
        status = draw_caption(clipped);
    }
    if (status == EDITOR_STATUS_OK) {
        status = draw_library(clipped);
    }
    if (status == EDITOR_STATUS_OK) {
        status = draw_stage(clipped);
    }
    if (status == EDITOR_STATUS_OK) {
        status = draw_transport(clipped);
    }
    if (status == EDITOR_STATUS_OK) {
        status = draw_properties(clipped);
    }
    if (status == EDITOR_STATUS_OK) {
        status = draw_timeline(clipped);
    }
    return status;
}

/* ============================================================== LIFE CYCLE */

static void copy_text(char *destination, size_t bytes, const char *source)
{
    size_t index = 0U;

    if (bytes == 0U) {
        return;
    }
    if (source != NULL) {
        while (index + 1U < bytes && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

enum editor_status editor_set_frame(struct ui_rect frame)
{
    const uint32_t least_width = EDITOR_BORDER * 2U + EDITOR_LIBRARY +
        EDITOR_PROPERTIES + 240U;
    const uint32_t least_height = EDITOR_BORDER * 2U + EDITOR_CAPTION +
        EDITOR_TRANSPORT + EDITOR_TIMELINE + 180U;

    if (frame.width < least_width || frame.height < least_height) {
        return EDITOR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return EDITOR_STATUS_OK;
}

enum editor_status editor_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum editor_status status;

    if (target == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    canvas = target;
    status = editor_set_frame(frame);
    if (status != EDITOR_STATUS_OK) {
        return status;
    }
    for (size_t index = 0U; index < EDITOR_MAX_ITEMS; ++index) {
        items[index] = (struct editor_item){ 0 };
    }
    clip = (struct editor_clip){ 0 };
    poster_pixels = NULL;
    poster_width = 0U;
    poster_height = 0U;
    selected = (size_t)-1;
    library_tab = EDITOR_TRACK_TEXT;
    playhead_ms = 0U;
    playing = false;
    hover_preset = (size_t)-1;
    hover_item = (size_t)-1;
    hover_tab = (size_t)-1;
    hover_field = (size_t)-1;
    hover_transport = false;
    hover_close = false;
    window_open = true;
    initialized = true;
    return EDITOR_STATUS_OK;
}

enum editor_status editor_set_clip(const struct editor_clip *source)
{
    if (source == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    copy_text(clip.name, sizeof(clip.name), source->name);
    clip.length_ms = source->length_ms;
    if (playhead_ms > clip.length_ms) {
        playhead_ms = clip.length_ms;
    }
    return EDITOR_STATUS_OK;
}

enum editor_status editor_set_poster(const uint32_t *pixels, uint32_t width,
    uint32_t height)
{
    if (pixels == NULL || width == 0U || height == 0U) {
        poster_pixels = NULL;
        poster_width = 0U;
        poster_height = 0U;
        return EDITOR_STATUS_OK;
    }
    poster_pixels = pixels;
    poster_width = width;
    poster_height = height;
    return EDITOR_STATUS_OK;
}

enum editor_status editor_set_item(size_t index,
    const struct editor_item *source)
{
    if (index >= EDITOR_MAX_ITEMS) {
        return EDITOR_STATUS_BAD_INDEX;
    }
    if (source == NULL) {
        items[index] = (struct editor_item){ 0 };
        if (selected == index) {
            selected = (size_t)-1;
        }
        return EDITOR_STATUS_OK;
    }
    if (source->track >= EDITOR_TRACK_COUNT ||
            source->style >= EDITOR_STYLE_COUNT ||
            source->effect >= EDITOR_EFFECT_COUNT ||
            source->strength > 100U) {
        return EDITOR_STATUS_BAD_INDEX;
    }
    items[index] = *source;
    items[index].label[EDITOR_TEXT_BYTES - 1U] = '\0';
    return EDITOR_STATUS_OK;
}

const struct editor_item *editor_item(size_t index)
{
    if (index >= EDITOR_MAX_ITEMS || !items[index].present) {
        return NULL;
    }
    return &items[index];
}

size_t editor_item_count(void)
{
    size_t counted = 0U;

    for (size_t index = 0U; index < EDITOR_MAX_ITEMS; ++index) {
        if (items[index].present) {
            ++counted;
        }
    }
    return counted;
}

size_t editor_selected(void)
{
    return selected;
}

enum editor_status editor_select(size_t index, struct ui_rect *damage)
{
    if (damage == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (index != (size_t)-1 && (index >= EDITOR_MAX_ITEMS ||
            !items[index].present)) {
        return EDITOR_STATUS_BAD_INDEX;
    }
    if (selected == index) {
        return EDITOR_STATUS_OK;
    }
    selected = index;
    *damage = join(properties_rect(), timeline_rect());
    return EDITOR_STATUS_OK;
}

uint32_t editor_playhead_ms(void)
{
    return playhead_ms;
}

enum editor_status editor_seek(uint32_t position_ms, struct ui_rect *damage)
{
    const uint32_t clamped = position_ms > clip.length_ms ? clip.length_ms :
        position_ms;

    if (damage == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (clamped == playhead_ms) {
        return EDITOR_STATUS_OK;
    }
    playhead_ms = clamped;
    played_from_ms = clamped;
    played_from_ns = clock_monotonic_ns();
    /* The stage as well as the timeline: the whole point of moving the
     * playhead is that the picture changes. */
    *damage = join(join(editor_stage_rect(), transport_rect()),
        timeline_rect());
    return EDITOR_STATUS_OK;
}

bool editor_playing(void)
{
    return playing;
}

enum editor_status editor_set_playing(bool wanted, struct ui_rect *damage)
{
    if (damage == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (playing == wanted) {
        return EDITOR_STATUS_OK;
    }
    playing = wanted;
    played_from_ms = playhead_ms;
    played_from_ns = clock_monotonic_ns();
    *damage = transport_rect();
    return EDITOR_STATUS_OK;
}

enum editor_status editor_open(struct ui_rect *damage)
{
    if (damage == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (window_open) {
        return EDITOR_STATUS_OK;
    }
    window_open = true;
    *damage = window_rect;
    return EDITOR_STATUS_OK;
}

enum editor_status editor_close(struct ui_rect *damage)
{
    if (damage == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open) {
        return EDITOR_STATUS_OK;
    }
    window_open = false;
    playing = false;
    hover_close = false;
    hover_preset = (size_t)-1;
    hover_item = (size_t)-1;
    hover_tab = (size_t)-1;
    hover_field = (size_t)-1;
    hover_transport = false;
    *damage = window_rect;
    return EDITOR_STATUS_OK;
}

bool editor_is_open(void)
{
    return initialized && window_open;
}

/* ================================================================== INPUT */

/* The first free slot, or the end. */
static size_t free_slot(void)
{
    for (size_t index = 0U; index < EDITOR_MAX_ITEMS; ++index) {
        if (!items[index].present) {
            return index;
        }
    }
    return (size_t)-1;
}

/*
 * What a preset does when it is pressed: it puts one of itself on the
 * timeline at the playhead, and selects it so the properties panel is
 * showing the thing that just appeared.
 */
static enum editor_status add_preset(size_t preset, struct ui_rect *damage)
{
    const size_t slot = free_slot();
    struct editor_item entry = { 0 };
    uint32_t length = EDITOR_NEW_LENGTH_MS;

    if (slot == (size_t)-1) {
        return EDITOR_STATUS_FULL;
    }
    if (playhead_ms + length > clip.length_ms) {
        length = clip.length_ms > playhead_ms ?
            clip.length_ms - playhead_ms : EDITOR_LEAST_LENGTH_MS;
    }
    entry.present = true;
    entry.start_ms = playhead_ms;
    entry.length_ms = length < EDITOR_LEAST_LENGTH_MS ?
        EDITOR_LEAST_LENGTH_MS : length;
    entry.strength = 100U;
    if (library_tab == EDITOR_TRACK_TEXT) {
        entry.track = EDITOR_TRACK_TEXT;
        entry.style = (enum editor_style)preset;
        copy_text(entry.label, sizeof(entry.label), style_sample[preset]);
    } else {
        entry.track = EDITOR_TRACK_EFFECT;
        entry.effect = (enum editor_effect)preset;
        copy_text(entry.label, sizeof(entry.label), effect_name[preset]);
    }
    items[slot] = entry;
    selected = slot;
    *damage = join(join(editor_stage_rect(), timeline_rect()),
        properties_rect());
    return EDITOR_STATUS_OK;
}

enum editor_status editor_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const size_t was_preset = hover_preset;
    const size_t was_item = hover_item;
    const size_t was_tab = hover_tab;
    const size_t was_field = hover_field;
    const bool was_transport = hover_transport;
    const bool was_close = hover_close;

    if (damage == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open) {
        return EDITOR_STATUS_OK;
    }
    hover_preset = (size_t)-1;
    hover_item = (size_t)-1;
    hover_tab = (size_t)-1;
    hover_field = (size_t)-1;
    hover_close = holds(caption_button_rect(2U), point);
    hover_transport = holds(transport_button_rect(), point);
    for (size_t index = 0U; index < 2U; ++index) {
        if (holds(library_tab_rect(index), point)) {
            hover_tab = index;
            break;
        }
    }
    for (size_t index = 0U; index < preset_count(); ++index) {
        if (holds(preset_rect(index), point)) {
            hover_preset = index;
            break;
        }
    }
    for (size_t index = 0U; index < field_count(); ++index) {
        if (holds(field_control_rect(index), point)) {
            hover_field = index;
            break;
        }
    }
    for (size_t index = 0U; index < EDITOR_MAX_ITEMS; ++index) {
        if (items[index].present && holds(item_rect(index), point)) {
            hover_item = index;
            break;
        }
    }
    if (was_tab != hover_tab || was_preset != hover_preset) {
        *damage = join(*damage, library_rect());
    }
    if (was_item != hover_item) {
        *damage = join(*damage, timeline_rect());
    }
    if (was_field != hover_field) {
        *damage = join(*damage, properties_rect());
    }
    if (was_transport != hover_transport) {
        *damage = join(*damage, transport_rect());
    }
    if (was_close != hover_close) {
        *damage = join(*damage, caption_button_rect(2U));
    }
    return EDITOR_STATUS_OK;
}

enum editor_status editor_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    struct ui_rect ruler;

    if (damage == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open) {
        return EDITOR_STATUS_OK;
    }
    if (holds(caption_button_rect(2U), point)) {
        return editor_close(damage);
    }
    /* The library's tab strip. */
    for (size_t index = 0U; index < 2U; ++index) {
        if (holds(library_tab_rect(index), point)) {
            const enum editor_track wanted = index == 0U ?
                EDITOR_TRACK_TEXT : EDITOR_TRACK_EFFECT;

            if (library_tab != wanted) {
                library_tab = wanted;
                hover_preset = (size_t)-1;
                *damage = library_rect();
            }
            return EDITOR_STATUS_OK;
        }
    }
    /* A preset, which adds one of itself. */
    for (size_t index = 0U; index < preset_count(); ++index) {
        if (holds(preset_rect(index), point)) {
            return add_preset(index, damage);
        }
    }
    if (holds(transport_button_rect(), point)) {
        return editor_set_playing(!playing, damage);
    }
    /* The scrub bar under the player, and the ruler over the lanes: both
     * seek, because both are drawn as the same time. */
    if (holds((struct ui_rect){ scrub_rect().x,
            transport_rect().y, scrub_rect().width,
            transport_rect().height }, point) && scrub_rect().width != 0U) {
        return editor_seek(x_to_time(scrub_rect(), point.x), damage);
    }
    ruler = ruler_rect();
    if (holds(ruler, point)) {
        return editor_seek(x_to_time(ruler, point.x), damage);
    }
    /* An item on the timeline. */
    for (size_t index = 0U; index < EDITOR_MAX_ITEMS; ++index) {
        if (items[index].present && holds(item_rect(index), point)) {
            return editor_select(index, damage);
        }
    }
    /* The properties panel's controls. */
    for (size_t index = 0U; index < field_count(); ++index) {
        const struct ui_rect control = field_control_rect(index);
        struct editor_item *entry;

        if (!holds(control, point)) {
            continue;
        }
        entry = &items[selected];
        if (index == 1U && entry->track == EDITOR_TRACK_TEXT) {
            entry->style = (enum editor_style)((entry->style + 1U) %
                EDITOR_STYLE_COUNT);
            copy_text(entry->label, sizeof(entry->label),
                style_sample[entry->style]);
            *damage = join(editor_stage_rect(), properties_rect());
            return EDITOR_STATUS_OK;
        }
        if (field_count() == 4U ? index == 2U : index == 1U) {
            const uint32_t span = control.width - EDITOR_SLIDER_THUMB - 40U;
            uint32_t value;

            if (span == 0U || point.x <= (int32_t)control.x) {
                value = 0U;
            } else if ((uint32_t)point.x >= control.x + span) {
                value = 100U;
            } else {
                value = ((uint32_t)point.x - control.x) * 100U / span;
            }
            entry->strength = (uint8_t)value;
            *damage = join(editor_stage_rect(), properties_rect());
            return EDITOR_STATUS_OK;
        }
        if (index + 1U == field_count()) {
            items[selected] = (struct editor_item){ 0 };
            selected = (size_t)-1;
            *damage = join(join(editor_stage_rect(), timeline_rect()),
                properties_rect());
            return EDITOR_STATUS_OK;
        }
    }
    /* A press on an empty lane deselects, the way clicking off a clip in
     * any editor does. */
    for (size_t track = 0U; track < EDITOR_TRACK_COUNT; ++track) {
        if (holds(lane_rect(track), point)) {
            return editor_select((size_t)-1, damage);
        }
    }
    return EDITOR_STATUS_OK;
}

enum editor_status editor_key_escape(struct ui_rect *damage)
{
    if (damage == NULL) {
        return EDITOR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EDITOR_STATUS_NOT_INITIALIZED;
    }
    if (!window_open) {
        return EDITOR_STATUS_OK;
    }
    if (playing) {
        return editor_set_playing(false, damage);
    }
    if (selected != (size_t)-1) {
        return editor_select((size_t)-1, damage);
    }
    return EDITOR_STATUS_OK;
}

/* ================================================================= MOTION */

bool editor_animating(void)
{
    return initialized && window_open && playing;
}

bool editor_animate(struct ui_rect *damage)
{
    uint64_t now;
    uint64_t elapsed_ms;
    uint32_t wanted;

    if (damage == NULL) {
        return false;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!editor_animating()) {
        return false;
    }
    now = clock_monotonic_ns();
    elapsed_ms = now > played_from_ns ?
        (now - played_from_ns) / UINT64_C(1000000) : 0U;
    wanted = played_from_ms + (uint32_t)elapsed_ms;
    if (wanted >= clip.length_ms) {
        /* Stopping at the end rather than looping: a player that wraps
         * round without being asked to is a player you cannot leave
         * parked on its last frame. */
        wanted = clip.length_ms;
        playing = false;
    }
    if (wanted == playhead_ms) {
        return false;
    }
    playhead_ms = wanted;
    *damage = join(join(editor_stage_rect(), transport_rect()),
        timeline_rect());
    return true;
}

/* ============================================================== SELF-TEST */

const char *editor_self_test_failure(void)
{
    return self_test_failure;
}

bool editor_self_test(void)
{
    struct surface *saved_canvas = canvas;
    const struct ui_rect saved_window = window_rect;
    const bool saved_initialized = initialized;
    const bool saved_open = window_open;
    const struct editor_clip saved_clip = clip;
    const size_t saved_selected = selected;
    const uint32_t saved_playhead = playhead_ms;
    struct editor_item saved_items[EDITOR_MAX_ITEMS];
    struct ui_rect damage;

    for (size_t index = 0U; index < EDITOR_MAX_ITEMS; ++index) {
        saved_items[index] = items[index];
        items[index] = (struct editor_item){ 0 };
    }
    initialized = true;
    window_open = true;
    window_rect = (struct ui_rect){ 0U, 0U, EDITOR_WIDTH, EDITOR_HEIGHT };
    clip.length_ms = 12000U;
    playhead_ms = 0U;
    selected = (size_t)-1;
    library_tab = EDITOR_TRACK_TEXT;

    /* Every mark this window names has to resolve to a rasterized cell. */
    {
        static const char *const wanted[] = { "type", "sparkle", "play",
            "pause", "plus", "trash-2", "chevron-down", "video" };

        for (size_t index = 0U; index < sizeof(wanted) / sizeof(wanted[0]);
             ++index) {
            uint32_t size = 0U;

            if (glyph_cell(wanted[index], 16U, &size) == NULL) {
                self_test_failure = "an editor mark has no glyph behind it";
                goto restore;
            }
        }
    }
    /*
     * The three panels have to tile the middle without overlapping.  A
     * library and a properties panel that between them are wider than the
     * window would leave the player with a negative width, which unsigned
     * arithmetic turns into an enormous one.
     */
    {
        const struct ui_rect middle = middle_rect();
        const struct ui_rect left = library_rect();
        const struct ui_rect player = player_rect();
        const struct ui_rect right = properties_rect();

        if (left.x + left.width != player.x ||
                player.x + player.width != right.x ||
                right.x + right.width != middle.x + middle.width ||
                player.width == 0U || player.width > middle.width) {
            self_test_failure = "the editor's three panels do not tile";
            goto restore;
        }
    }
    /* The stage and the transport have to tile the player the same way. */
    {
        const struct ui_rect player = player_rect();
        const struct ui_rect stage = editor_stage_rect();
        const struct ui_rect bar = transport_rect();

        if (stage.y + stage.height != bar.y ||
                bar.y + bar.height != player.y + player.height ||
                stage.height == 0U) {
            self_test_failure = "the editor's player does not tile";
            goto restore;
        }
    }
    /* Time to a column and back has to round-trip at both ends. */
    {
        const struct ui_rect ruler = ruler_rect();

        if (x_to_time(ruler, (int32_t)ruler.x) != 0U ||
                x_to_time(ruler, (int32_t)(ruler.x + ruler.width)) !=
                    clip.length_ms ||
                time_to_x(ruler, 0U) != ruler.x ||
                time_to_x(ruler, clip.length_ms) !=
                    ruler.x + ruler.width) {
            self_test_failure = "the editor's timeline does not map time";
            goto restore;
        }
    }
    /* A preset has to actually add something, on the lane it belongs to. */
    {
        library_tab = EDITOR_TRACK_TEXT;
        if (add_preset(0U, &damage) != EDITOR_STATUS_OK ||
                editor_item_count() != 1U ||
                items[0].track != EDITOR_TRACK_TEXT ||
                selected != 0U) {
            self_test_failure = "a text preset added nothing";
            goto restore;
        }
        library_tab = EDITOR_TRACK_EFFECT;
        if (add_preset(EDITOR_EFFECT_MONO, &damage) != EDITOR_STATUS_OK ||
                editor_item_count() != 2U ||
                items[1].track != EDITOR_TRACK_EFFECT ||
                items[1].effect != EDITOR_EFFECT_MONO) {
            self_test_failure = "an effect preset added nothing";
            goto restore;
        }
    }
    /* And what it adds has to be reachable: on the lane, inside the clip. */
    {
        const struct ui_rect box = item_rect(1U);
        const struct ui_rect lane = lane_rect(EDITOR_TRACK_EFFECT);

        if (box.width == 0U || box.x < lane.x ||
                box.x + box.width > lane.x + lane.width ||
                box.y != lane.y) {
            self_test_failure = "an added item is not on its own lane";
            goto restore;
        }
    }
    /* An effect at the playhead has to change the picture, and one that is
     * not at the playhead has to leave it alone.  This is the check that
     * would have caught an effect wired to nothing. */
    {
        const struct editor_mix source = { 200U, 120U, 60U };
        struct editor_mix mixed = apply_effect(source, EDITOR_EFFECT_MONO,
            255U, 10U, 10U, 100U, 100U);

        if (mixed.red != mixed.green || mixed.green != mixed.blue) {
            self_test_failure = "the editor's mono effect is not grey";
            goto restore;
        }
        mixed = apply_effect(source, EDITOR_EFFECT_MONO, 0U, 10U, 10U, 100U,
            100U);
        if (mixed.red != source.red || mixed.blue != source.blue) {
            self_test_failure = "an effect at no strength still changed the "
                "picture";
            goto restore;
        }
        mixed = apply_effect(source, EDITOR_EFFECT_VIGNETTE, 255U, 50U, 50U,
            100U, 100U);
        if (mixed.red != source.red) {
            self_test_failure = "the editor's vignette darkens its centre";
            goto restore;
        }
    }
    /* An item covers the playhead or it does not, and the boundaries are
     * half open - an item ending at 2000 is not active at 2000. */
    {
        uint32_t weight = 0U;

        items[0].start_ms = 1000U;
        items[0].length_ms = 1000U;
        items[0].strength = 100U;
        playhead_ms = 999U;
        if (item_active(&items[0], &weight)) {
            self_test_failure = "an item was active before it starts";
            goto restore;
        }
        playhead_ms = 1000U;
        if (!item_active(&items[0], &weight) || weight != 255U) {
            self_test_failure = "an item was not active at its own start";
            goto restore;
        }
        playhead_ms = 2000U;
        if (item_active(&items[0], &weight)) {
            self_test_failure = "an item was still active at its own end";
            goto restore;
        }
    }
    /* Seeking clamps rather than running off the end of the clip. */
    {
        (void)editor_seek(999999U, &damage);
        if (playhead_ms != clip.length_ms) {
            self_test_failure = "the editor seeked past the end of its clip";
            goto restore;
        }
    }
    /*
     * Playing advances the playhead by real elapsed time, and stops at the
     * end rather than wrapping.  Backdating the start rather than sleeping
     * is what makes this testable at all: a self-test that waits a second
     * is a self-test nobody runs.
     */
    {
        struct ui_rect moved;
        const uint64_t now = clock_monotonic_ns();
        /*
         * However far back the clock lets us reach.  A caller may have
         * PINNED it near zero to make its own animation frames repeatable,
         * so a fixed backdate is not available: subtracting more than the
         * clock holds clamps to zero and the elapsed time becomes whatever
         * the clock happened to be, which is how the first version of this
         * check passed on a running clock and failed on a pinned one.
         */
        const uint64_t back = now > UINT64_C(2000000000) ?
            UINT64_C(2000000000) : now;

        window_open = true;
        if (back >= UINT64_C(1000000)) {
            const uint32_t expected = (uint32_t)(back / UINT64_C(1000000));

            playhead_ms = 0U;
            played_from_ms = 0U;
            playing = true;
            played_from_ns = now - back;
            /* Two milliseconds of slack: editor_animate() reads the clock
             * again, and it has moved on since `now`. */
            if (!editor_animate(&moved) || playhead_ms < expected ||
                    playhead_ms > expected + 2U) {
                self_test_failure = "playing did not advance the playhead";
                goto restore;
            }
        }
        /*
         * And a play that STARTS at the end parks on the last frame with
         * the transport stopped, rather than wrapping round.  This one
         * needs no backdate at all, so it runs whatever the clock says.
         */
        playhead_ms = 0U;
        played_from_ms = clip.length_ms;
        playing = true;
        played_from_ns = now;
        (void)editor_animate(&moved);
        if (playhead_ms != clip.length_ms || playing) {
            self_test_failure = "playing ran past the end of the clip";
            goto restore;
        }
    }
    /* Every field the properties panel draws has to be inside the panel. */
    {
        selected = 0U;
        for (size_t index = 0U; index < field_count(); ++index) {
            const struct ui_rect row = field_rect(index);
            const struct ui_rect area = properties_rect();

            if (row.width == 0U || row.x < area.x ||
                    row.x + row.width > area.x + area.width ||
                    row.y + row.height > area.y + area.height) {
                self_test_failure = "a properties row is outside its panel";
                goto restore;
            }
        }
    }
    self_test_failure = "";

restore:
    canvas = saved_canvas;
    window_rect = saved_window;
    initialized = saved_initialized;
    window_open = saved_open;
    clip = saved_clip;
    selected = saved_selected;
    playhead_ms = saved_playhead;
    playing = false;
    library_tab = EDITOR_TRACK_TEXT;
    for (size_t index = 0U; index < EDITOR_MAX_ITEMS; ++index) {
        items[index] = saved_items[index];
    }
    return self_test_failure[0] == '\0';
}
