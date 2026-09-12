/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * A message dialog.  See include/phipia/dialog.h for the shape.
 */

#include <phipia/dialog.h>

#include <phipia/framebuffer.h>
#include <phipia/ui_font.h>

#include "dialog_art.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION: Windows 10's task dialog at 100% scaling, read off
 * the window rather than measured against a published sheet.  The
 * ARRANGEMENT is certain and is the thing worth copying: caption with a
 * close mark and no other button, a white body with the icon at its left,
 * a grey command strip under a hairline, buttons right-aligned along it and
 * the default one carrying the accent.
 *
 * What is deliberately NOT Windows' is the treatment.  Its buttons are grey
 * plates with a grey border; these follow the same rule the rest of this
 * shell's buttons do - a near-white plate, a one-pixel edge that goes accent
 * under the pointer - so the dialog belongs to Phipia rather than sitting in
 * it as a quotation.
 */

#define DIALOG_BORDER 1U
#define DIALOG_CAPTION 32U
#define DIALOG_CAPTION_BUTTON 46U
#define DIALOG_CAPTION_MARK 10U
#define DIALOG_WIDTH 420U
/* The body: the icon's box, the margin round it, and the text beside it. */
#define DIALOG_PAD 22U
#define DIALOG_ICON 32U
#define DIALOG_ICON_GAP 16U
/*
 * The first baseline, measured from the body's top so that the message's
 * CAP TOP lands level with the top of the icon beside it - which is where
 * Windows puts it, and what stops a 32-pixel mark reading as sitting below
 * a 15-pixel line.  Twelve is the font's cap height at this size; the
 * self-test checks the two still line up if the font service hands out a
 * different one.
 */
#define DIALOG_MESSAGE_TOP (DIALOG_PAD + 12U)
#define DIALOG_DETAIL_DROP 22U    /* and the second, under it            */
#define DIALOG_BODY_LEAST 76U
/* The command strip. */
#define DIALOG_STRIP 52U
#define DIALOG_BUTTON_WIDTH 88U
#define DIALOG_BUTTON_HEIGHT 26U
#define DIALOG_BUTTON_GAP 8U

/* ================================================================ PALETTE
 *
 * The light window palette the rest of the shell's windows use.  A dialog
 * has no dark variant in Windows 10 either - the message box stayed light
 * through every theme it shipped with.
 */

struct dialog_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define DIALOG_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct dialog_rgb body_fill = DIALOG_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct dialog_rgb chrome = DIALOG_RGB(0xF0U, 0xF0U, 0xF0U);
static const struct dialog_rgb border = DIALOG_RGB(0x00U, 0x78U, 0xD4U);
static const struct dialog_rgb rule = DIALOG_RGB(0xE1U, 0xE1U, 0xE1U);
static const struct dialog_rgb ink = DIALOG_RGB(0x1AU, 0x1AU, 0x1AU);
static const struct dialog_rgb ink_soft = DIALOG_RGB(0x60U, 0x60U, 0x60U);
static const struct dialog_rgb button_fill = DIALOG_RGB(0xFDU, 0xFDU, 0xFDU);
static const struct dialog_rgb button_hot = DIALOG_RGB(0xE5U, 0xF1U, 0xFBU);
static const struct dialog_rgb button_edge = DIALOG_RGB(0xADU, 0xADU, 0xADU);
static const struct dialog_rgb accent = DIALOG_RGB(0x00U, 0x78U, 0xD4U);
static const struct dialog_rgb close_hot = DIALOG_RGB(0xE8U, 0x11U, 0x23U);

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect screen_rect;
static struct ui_rect window_rect;
static bool initialized;
static bool open;
static struct dialog_request current;
static size_t hover_button = (size_t)-1;
static bool hover_close;
static enum dialog_answer answer;
static size_t answered_button;
static const char *self_test_failure = "";

const char *dialog_status_string(enum dialog_status status)
{
    switch (status) {
    case DIALOG_STATUS_OK:
        return "ok";
    case DIALOG_STATUS_NULL_ARGUMENT:
        return "null argument";
    case DIALOG_STATUS_NOT_INITIALIZED:
        return "dialog not initialized";
    case DIALOG_STATUS_BAD_INDEX:
        return "dialog index is out of range";
    case DIALOG_STATUS_UNSUPPORTED_GEOMETRY:
        return "dialog geometry is unsupported";
    case DIALOG_STATUS_SURFACE_FAILURE:
        return "dialog surface refused a pixel";
    default:
        return "unknown dialog status";
    }
}

/* ============================================================== PRIMITIVES */

static uint32_t pack_rgb(struct dialog_rgb colour)
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

static bool holds(struct ui_rect area, struct ui_point point)
{
    return point.x >= 0 && point.y >= 0 &&
        (uint32_t)point.x >= area.x &&
        (uint32_t)point.x < area.x + area.width &&
        (uint32_t)point.y >= area.y &&
        (uint32_t)point.y < area.y + area.height;
}

static enum dialog_status fill(struct ui_rect area, struct ui_rect damage,
    struct dialog_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return DIALOG_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return DIALOG_STATUS_OK;
}

/* A one-pixel edge round a box, which is every border this window draws. */
static enum dialog_status outline(struct ui_rect box, struct ui_rect damage,
    struct dialog_rgb colour)
{
    enum dialog_status status;

    if (box.width == 0U || box.height == 0U) {
        return DIALOG_STATUS_OK;
    }
    status = fill((struct ui_rect){ box.x, box.y, box.width, 1U }, damage,
        colour);
    if (status == DIALOG_STATUS_OK) {
        status = fill((struct ui_rect){ box.x, box.y + box.height - 1U,
            box.width, 1U }, damage, colour);
    }
    if (status == DIALOG_STATUS_OK) {
        status = fill((struct ui_rect){ box.x, box.y, 1U, box.height },
            damage, colour);
    }
    if (status == DIALOG_STATUS_OK) {
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

static enum dialog_status text_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct dialog_rgb colour)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (clip.width == 0U || clip.height == 0U || body == NULL ||
            body[0] == '\0') {
        return DIALOG_STATUS_OK;
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
    return DIALOG_STATUS_OK;
}

/*
 * The mark, drawn one-to-one from the largest plane that fits its box.
 * Nothing is resampled here for the same reason nothing is resampled
 * anywhere else in this shell: a picture reduced twice is blurred twice.
 */
static enum dialog_status draw_mark(const char *name, struct ui_rect box,
    struct ui_rect damage)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t wanted = box.width < box.height ? box.width : box.height;
    const uint32_t *pixels = NULL;
    const uint8_t *alpha = NULL;
    uint32_t side = 0U;
    struct ui_rect placed;
    struct ui_rect clipped;

    for (size_t index = 0U; index < DIALOG_ART_COUNT; ++index) {
        const char *left = dialog_art[index].name;
        const char *right = name;

        while (*left != '\0' && *left == *right) {
            ++left;
            ++right;
        }
        if (*left != '\0' || *right != '\0') {
            continue;
        }
        for (size_t option = 0U; option < DIALOG_ART_SIZES; ++option) {
            if (dialog_art_size[option] <= wanted) {
                pixels = dialog_art[index].pixels[option];
                alpha = dialog_art[index].alpha[option];
                side = dialog_art_size[option];
            }
        }
        break;
    }
    if (pixels == NULL || alpha == NULL || side == 0U) {
        return DIALOG_STATUS_OK;
    }
    placed = (struct ui_rect){
        box.x + (box.width > side ? (box.width - side) / 2U : 0U),
        box.y + (box.height > side ? (box.height - side) / 2U : 0U),
        side, side };
    clipped = intersect(intersect(placed, damage), window_rect);
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
                return DIALOG_STATUS_SURFACE_FAILURE;
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
                return DIALOG_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return DIALOG_STATUS_OK;
}

/* ================================================================= LAYOUT */

static const char *icon_name(void)
{
    switch (current.icon) {
    case DIALOG_ICON_ERROR:
        return "error";
    case DIALOG_ICON_WARNING:
        return "warning";
    case DIALOG_ICON_NONE:
    default:
        return NULL;
    }
}

/*
 * How tall the body has to be for what is in it.  One line of message needs
 * less than a message with a detail under it, and both need at least as much
 * as the icon beside them - a body shorter than its own mark is the defect
 * this measures away.
 */
static uint32_t body_height(void)
{
    uint32_t wanted = DIALOG_MESSAGE_TOP + 10U;

    if (current.detail[0] != '\0') {
        wanted += DIALOG_DETAIL_DROP;
    }
    wanted += DIALOG_PAD;
    if (icon_name() != NULL && wanted < DIALOG_PAD * 2U + DIALOG_ICON) {
        wanted = DIALOG_PAD * 2U + DIALOG_ICON;
    }
    return wanted < DIALOG_BODY_LEAST ? DIALOG_BODY_LEAST : wanted;
}

struct ui_rect dialog_bounds(void)
{
    const uint32_t height = DIALOG_BORDER * 2U + DIALOG_CAPTION +
        body_height() + DIALOG_STRIP;
    const uint32_t width = DIALOG_WIDTH;
    uint32_t x = screen_rect.x;
    uint32_t y = screen_rect.y;

    if (screen_rect.width > width) {
        x += (screen_rect.width - width) / 2U;
    }
    if (screen_rect.height > height) {
        y += (screen_rect.height - height) / 2U;
    }
    return (struct ui_rect){ x, y, width, height };
}

static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){ window_rect.x + DIALOG_BORDER,
        window_rect.y + DIALOG_BORDER,
        window_rect.width - DIALOG_BORDER * 2U, DIALOG_CAPTION };
}

static struct ui_rect close_rect(void)
{
    const struct ui_rect bar = caption_rect();

    return (struct ui_rect){ bar.x + bar.width - DIALOG_CAPTION_BUTTON, bar.y,
        DIALOG_CAPTION_BUTTON, bar.height };
}

static struct ui_rect body_rect(void)
{
    const struct ui_rect bar = caption_rect();

    return (struct ui_rect){ bar.x, bar.y + bar.height, bar.width,
        body_height() };
}

static struct ui_rect strip_rect(void)
{
    const struct ui_rect body = body_rect();

    return (struct ui_rect){ body.x, body.y + body.height, body.width,
        DIALOG_STRIP };
}

/*
 * The buttons, right to left in the strip.  Index 0 is the RIGHTMOST, which
 * is where Windows puts the first one a task dialog is given and where the
 * eye lands last.
 */
static struct ui_rect button_rect(size_t index)
{
    const struct ui_rect strip = strip_rect();
    const uint32_t from_right = (uint32_t)(index + 1U) *
        DIALOG_BUTTON_WIDTH + (uint32_t)index * DIALOG_BUTTON_GAP +
        DIALOG_PAD;

    if (index >= current.buttons || strip.width < from_right) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ strip.x + strip.width - from_right,
        strip.y + (strip.height - DIALOG_BUTTON_HEIGHT) / 2U,
        DIALOG_BUTTON_WIDTH, DIALOG_BUTTON_HEIGHT };
}

/* ================================================================ DRAWING */

static enum dialog_status draw_caption(struct ui_rect damage)
{
    const struct ui_rect bar = caption_rect();
    const struct ui_rect button = close_rect();
    const uint32_t mid_x = button.x + button.width / 2U;
    const uint32_t mid_y = button.y + button.height / 2U;
    const uint32_t half = DIALOG_CAPTION_MARK / 2U;
    enum dialog_status status = fill(bar, damage, body_fill);

    if (status == DIALOG_STATUS_OK) {
        status = text_at(damage, bar.x + 12U, bar.y + bar.height / 2U + 5U,
            current.title, ink);
    }
    if (status == DIALOG_STATUS_OK && hover_close) {
        status = fill(button, damage, close_hot);
    }
    /*
     * The close mark, drawn rather than looked up, for the reason Task
     * Manager draws its own: a Lucide cross here is a 24-pixel mark with a
     * round-capped two-pixel stroke, and what Windows puts in this button is
     * ten pixels of one-pixel line.
     */
    for (uint32_t step = 0U; step < DIALOG_CAPTION_MARK &&
            status == DIALOG_STATUS_OK; ++step) {
        const struct dialog_rgb mark = hover_close ? body_fill : ink_soft;

        status = fill((struct ui_rect){ mid_x - half + step,
            mid_y - half + step, 1U, 1U }, damage, mark);
        if (status == DIALOG_STATUS_OK) {
            status = fill((struct ui_rect){ mid_x - half + step,
                mid_y + half - 1U - step, 1U, 1U }, damage, mark);
        }
    }
    return status;
}

static enum dialog_status draw_body(struct ui_rect damage)
{
    const struct ui_rect body = body_rect();
    const char *mark = icon_name();
    uint32_t text_x = body.x + DIALOG_PAD;
    enum dialog_status status = fill(body, damage, body_fill);

    if (status == DIALOG_STATUS_OK && mark != NULL) {
        status = draw_mark(mark, (struct ui_rect){ body.x + DIALOG_PAD,
            body.y + DIALOG_PAD, DIALOG_ICON, DIALOG_ICON }, damage);
        text_x += DIALOG_ICON + DIALOG_ICON_GAP;
    }
    if (status == DIALOG_STATUS_OK) {
        status = text_at(damage, text_x, body.y + DIALOG_MESSAGE_TOP,
            current.message, ink);
    }
    if (status == DIALOG_STATUS_OK && current.detail[0] != '\0') {
        status = text_at(damage, text_x,
            body.y + DIALOG_MESSAGE_TOP + DIALOG_DETAIL_DROP,
            current.detail, ink_soft);
    }
    return status;
}

static enum dialog_status draw_strip(struct ui_rect damage)
{
    const struct ui_rect strip = strip_rect();
    enum dialog_status status = fill(strip, damage, chrome);

    if (status == DIALOG_STATUS_OK) {
        status = fill((struct ui_rect){ strip.x, strip.y, strip.width, 1U },
            damage, rule);
    }
    for (size_t index = 0U; index < current.buttons &&
            status == DIALOG_STATUS_OK; ++index) {
        const struct ui_rect box = button_rect(index);
        const bool hot = index == hover_button;
        const bool first = index == current.defaulted;

        if (box.width == 0U) {
            continue;
        }
        status = fill(box, damage, hot ? button_hot : button_fill);
        if (status == DIALOG_STATUS_OK) {
            status = outline(box, damage, hot || first ? accent :
                button_edge);
        }
        if (status == DIALOG_STATUS_OK) {
            const uint32_t width = width_of(current.button[index]);

            status = text_at(damage,
                box.x + (box.width > width ? (box.width - width) / 2U : 0U),
                box.y + box.height / 2U + 5U, current.button[index], ink);
        }
    }
    return status;
}

enum dialog_status dialog_draw(struct ui_rect damage)
{
    enum dialog_status status;

    if (!initialized) {
        return DIALOG_STATUS_NOT_INITIALIZED;
    }
    if (!open) {
        return DIALOG_STATUS_OK;
    }
    window_rect = dialog_bounds();
    /*
     * The frame is the accent, one pixel all round.  Windows 10 gives a
     * dialog the same coloured border it gives an active window, which is
     * what says this one is in front and holding the keyboard.
     */
    status = fill(window_rect, intersect(damage, window_rect), border);
    if (status == DIALOG_STATUS_OK) {
        status = draw_caption(damage);
    }
    if (status == DIALOG_STATUS_OK) {
        status = draw_body(damage);
    }
    if (status == DIALOG_STATUS_OK) {
        status = draw_strip(damage);
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

enum dialog_status dialog_set_screen(struct ui_rect screen)
{
    if (screen.width < DIALOG_WIDTH ||
            screen.height < DIALOG_CAPTION + DIALOG_BODY_LEAST +
                DIALOG_STRIP + DIALOG_BORDER * 2U) {
        return DIALOG_STATUS_UNSUPPORTED_GEOMETRY;
    }
    screen_rect = screen;
    window_rect = dialog_bounds();
    return DIALOG_STATUS_OK;
}

enum dialog_status dialog_initialize(struct surface *target,
    struct ui_rect screen)
{
    enum dialog_status status;

    if (target == NULL) {
        return DIALOG_STATUS_NULL_ARGUMENT;
    }
    canvas = target;
    status = dialog_set_screen(screen);
    if (status != DIALOG_STATUS_OK) {
        return status;
    }
    current = (struct dialog_request){ 0 };
    open = false;
    hover_button = (size_t)-1;
    hover_close = false;
    answer = DIALOG_ANSWER_NONE;
    answered_button = 0U;
    initialized = true;
    return DIALOG_STATUS_OK;
}

enum dialog_status dialog_open(const struct dialog_request *request,
    struct ui_rect *damage)
{
    struct ui_rect was;

    if (request == NULL || damage == NULL) {
        return DIALOG_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return DIALOG_STATUS_NOT_INITIALIZED;
    }
    if (request->buttons == 0U || request->buttons > DIALOG_MAX_BUTTONS ||
            request->defaulted >= request->buttons) {
        return DIALOG_STATUS_BAD_INDEX;
    }
    was = open ? window_rect : (struct ui_rect){ 0U, 0U, 0U, 0U };
    current = (struct dialog_request){ 0 };
    copy_text(current.title, sizeof(current.title), request->title);
    copy_text(current.message, sizeof(current.message), request->message);
    copy_text(current.detail, sizeof(current.detail), request->detail);
    current.icon = request->icon;
    current.buttons = request->buttons;
    current.defaulted = request->defaulted;
    for (size_t index = 0U; index < request->buttons; ++index) {
        copy_text(current.button[index], sizeof(current.button[index]),
            request->button[index]);
    }
    open = true;
    hover_button = (size_t)-1;
    hover_close = false;
    answer = DIALOG_ANSWER_NONE;
    window_rect = dialog_bounds();
    *damage = was.width == 0U ? window_rect : (struct ui_rect){
        was.x < window_rect.x ? was.x : window_rect.x,
        was.y < window_rect.y ? was.y : window_rect.y,
        was.width > window_rect.width ? was.width : window_rect.width,
        was.height > window_rect.height ? was.height : window_rect.height };
    return DIALOG_STATUS_OK;
}

enum dialog_status dialog_close(struct ui_rect *damage)
{
    if (damage == NULL) {
        return DIALOG_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return DIALOG_STATUS_NOT_INITIALIZED;
    }
    if (!open) {
        return DIALOG_STATUS_OK;
    }
    *damage = window_rect;
    open = false;
    hover_button = (size_t)-1;
    hover_close = false;
    return DIALOG_STATUS_OK;
}

bool dialog_is_open(void)
{
    return initialized && open;
}

enum dialog_answer dialog_take_answer(size_t *button_out)
{
    const enum dialog_answer taken = answer;

    if (button_out != NULL) {
        *button_out = answered_button;
    }
    answer = DIALOG_ANSWER_NONE;
    return taken;
}

/* ================================================================== INPUT */

enum dialog_status dialog_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    size_t wanted = (size_t)-1;
    bool over_close;

    if (damage == NULL) {
        return DIALOG_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return DIALOG_STATUS_NOT_INITIALIZED;
    }
    if (!open) {
        return DIALOG_STATUS_OK;
    }
    for (size_t index = 0U; index < current.buttons; ++index) {
        if (holds(button_rect(index), point)) {
            wanted = index;
            break;
        }
    }
    over_close = holds(close_rect(), point);
    if (wanted == hover_button && over_close == hover_close) {
        return DIALOG_STATUS_OK;
    }
    hover_button = wanted;
    hover_close = over_close;
    *damage = window_rect;
    return DIALOG_STATUS_OK;
}

enum dialog_status dialog_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return DIALOG_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return DIALOG_STATUS_NOT_INITIALIZED;
    }
    if (!open) {
        return DIALOG_STATUS_OK;
    }
    for (size_t index = 0U; index < current.buttons; ++index) {
        if (holds(button_rect(index), point)) {
            answer = DIALOG_ANSWER_BUTTON;
            answered_button = index;
            return dialog_close(damage);
        }
    }
    if (holds(close_rect(), point)) {
        answer = DIALOG_ANSWER_DISMISSED;
        return dialog_close(damage);
    }
    /*
     * A press anywhere else inside the dialog is swallowed rather than
     * passed on, because that is what modal means; a press outside it does
     * nothing at all, which is Windows' answer too - it does not close on a
     * click away.
     */
    return DIALOG_STATUS_OK;
}

enum dialog_status dialog_key_escape(struct ui_rect *damage)
{
    if (damage == NULL) {
        return DIALOG_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return DIALOG_STATUS_NOT_INITIALIZED;
    }
    if (!open) {
        return DIALOG_STATUS_OK;
    }
    answer = DIALOG_ANSWER_DISMISSED;
    return dialog_close(damage);
}

enum dialog_status dialog_key_return(struct ui_rect *damage)
{
    if (damage == NULL) {
        return DIALOG_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return DIALOG_STATUS_NOT_INITIALIZED;
    }
    if (!open) {
        return DIALOG_STATUS_OK;
    }
    answer = DIALOG_ANSWER_BUTTON;
    answered_button = current.defaulted;
    return dialog_close(damage);
}

/* ============================================================== SELF-TEST */

const char *dialog_self_test_failure(void)
{
    return self_test_failure;
}

bool dialog_self_test(void)
{
    struct dialog_request probe = { 0 };
    struct ui_rect damage;
    struct surface *saved_canvas = canvas;
    struct ui_rect saved_screen = screen_rect;
    const bool saved_initialized = initialized;
    const bool saved_open = open;
    const struct dialog_request saved_current = current;

    initialized = true;
    screen_rect = (struct ui_rect){ 0U, 0U, 1280U, 800U };
    open = false;

    /* A dialog with no button is a dialog nothing can dismiss. */
    copy_text(probe.title, sizeof(probe.title), "Probe");
    copy_text(probe.message, sizeof(probe.message), "Probe");
    probe.buttons = 0U;
    if (dialog_open(&probe, &damage) != DIALOG_STATUS_BAD_INDEX) {
        self_test_failure = "the dialog accepted no buttons at all";
        goto restore;
    }
    probe.buttons = DIALOG_MAX_BUTTONS + 1U;
    if (dialog_open(&probe, &damage) != DIALOG_STATUS_BAD_INDEX) {
        self_test_failure = "the dialog accepted more buttons than it draws";
        goto restore;
    }
    /* And a default that is not one of them defaults to nothing. */
    probe.buttons = 2U;
    probe.defaulted = 2U;
    if (dialog_open(&probe, &damage) != DIALOG_STATUS_BAD_INDEX) {
        self_test_failure = "the dialog defaulted to a button it has not got";
        goto restore;
    }
    probe.defaulted = 0U;
    copy_text(probe.button[0], sizeof(probe.button[0]), "OK");
    copy_text(probe.button[1], sizeof(probe.button[1]), "Cancel");
    if (dialog_open(&probe, &damage) != DIALOG_STATUS_OK || !dialog_is_open()) {
        self_test_failure = "the dialog refused a request it should take";
        goto restore;
    }
    /*
     * The buttons run right to left and must not overlap or leave the
     * strip.  Two 88-pixel buttons and a gap in a 420-pixel window is the
     * case that would break first if any of those numbers moved.
     */
    for (size_t index = 0U; index + 1U < probe.buttons; ++index) {
        const struct ui_rect right = button_rect(index);
        const struct ui_rect left = button_rect(index + 1U);

        if (right.width == 0U || left.width == 0U ||
                left.x + left.width > right.x) {
            self_test_failure = "two dialog buttons overlap";
            goto restore;
        }
    }
    if (button_rect(probe.buttons - 1U).x < strip_rect().x + DIALOG_PAD) {
        self_test_failure = "a dialog button runs off its strip";
        goto restore;
    }
    /* The body has to be at least as tall as the mark standing in it. */
    probe.icon = DIALOG_ICON_ERROR;
    current.icon = DIALOG_ICON_ERROR;
    if (body_height() < DIALOG_PAD * 2U + DIALOG_ICON) {
        self_test_failure = "the dialog body is shorter than its own icon";
        goto restore;
    }
    /*
     * The message's cap top has to sit level with the icon's, or a 32-pixel
     * mark reads as slumped beside a line of type.  The font service's
     * ascent is measured from the baseline to the top of the CELL, and the
     * cap sits a little under that; anything within three pixels reads as
     * level, and more than that does not.
     */
    {
        const struct ui_font_metrics font = ui_font_get_metrics();
        const uint32_t cap_top = DIALOG_MESSAGE_TOP > font.ascent ?
            DIALOG_MESSAGE_TOP - font.ascent : 0U;
        const uint32_t drift = cap_top > DIALOG_PAD ? cap_top - DIALOG_PAD :
            DIALOG_PAD - cap_top;

        if (drift > 3U) {
            self_test_failure =
                "the dialog's message does not line up with its icon";
            goto restore;
        }
    }
    /* Escape answers, and answering once answers once. */
    if (dialog_key_escape(&damage) != DIALOG_STATUS_OK || dialog_is_open()) {
        self_test_failure = "Escape did not close the dialog";
        goto restore;
    }
    if (dialog_take_answer(NULL) != DIALOG_ANSWER_DISMISSED ||
            dialog_take_answer(NULL) != DIALOG_ANSWER_NONE) {
        self_test_failure = "the dialog answered twice for one dismissal";
        goto restore;
    }
    /* Return presses the default one, whichever index that is. */
    probe.defaulted = 1U;
    if (dialog_open(&probe, &damage) != DIALOG_STATUS_OK) {
        self_test_failure = "the dialog refused a request it should take";
        goto restore;
    }
    {
        size_t which = 0U;

        (void)dialog_key_return(&damage);
        if (dialog_take_answer(&which) != DIALOG_ANSWER_BUTTON ||
                which != 1U) {
            self_test_failure = "Return did not press the default button";
            goto restore;
        }
    }
    /* Both marks the header names have to resolve to artwork. */
    for (size_t index = 0U; index < DIALOG_ART_COUNT; ++index) {
        if (dialog_art[index].name == NULL) {
            self_test_failure = "a dialog mark has no artwork behind it";
            goto restore;
        }
    }
    if (DIALOG_ART_COUNT < 2U) {
        self_test_failure = "the dialog is missing one of its two marks";
        goto restore;
    }
    self_test_failure = "";

restore:
    canvas = saved_canvas;
    screen_rect = saved_screen;
    initialized = saved_initialized;
    open = saved_open;
    current = saved_current;
    answer = DIALOG_ANSWER_NONE;
    return self_test_failure[0] == '\0';
}
