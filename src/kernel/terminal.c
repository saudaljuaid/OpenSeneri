/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * A Windows 10 console window.
 *
 * See include/phipia/terminal.h for what this is and what is sourced.  The
 * short version: the palette is Microsoft's Campbell scheme, published with
 * the ColorTool that shipped with Windows 10 1709; the chrome is measured
 * off a Windows 10 window; the font is DejaVu Sans Mono because Consolas
 * cannot be redistributed.
 */

#include <phipia/terminal.h>

#include <phipia/cursor.h>

#include <phipia/clock.h>
#include <phipia/framebuffer.h>

#include "console_font.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION, all of them: read off a Windows 10 window at 100%
 * scaling rather than taken from a published resource.
 */

/* The caption.  Windows 10's standard title bar is thirty-two pixels tall
 * and its three buttons are forty-six wide - wider than they are tall, which
 * is the proportion that makes a Windows title bar look like one. */
#define TERMINAL_CAPTION_HEIGHT 32U
#define TERMINAL_CAPTION_BUTTON 46U
/* The window icon, set in from the left edge. */
#define TERMINAL_ICON_SIZE 16U
#define TERMINAL_ICON_INSET 8U
#define TERMINAL_TITLE_INSET 34U
/* Windows 10 draws a one-pixel border in the accent colour around an active
 * window, and a grey one around an inactive one.  It is the whole of the
 * window's edge: there is no second frame inside it. */
#define TERMINAL_BORDER 1U
/* The client area's own padding, which cmd.exe does not have and conhost
 * does: text does not start hard against the frame. */
#define TERMINAL_PADDING_X 4U
#define TERMINAL_PADDING_Y 2U
/* The scrollbar down the right-hand side, which is what says "console" as
 * loudly as the black does. */
#define TERMINAL_SCROLLBAR 16U
/* cmd.exe blinks its cursor at roughly two hertz. */
#define TERMINAL_BLINK_NS UINT64_C(530000000)
/* Its default cursor is a thick underline a quarter of the cell tall. */
#define TERMINAL_CURSOR_FRACTION 4U

/* =========================================================== TERMINAL PALETTE
 *
 * Campbell, exactly as Microsoft published it with the Windows 10 ColorTool.
 * These are not approximations and they are not a theme: a Command Prompt on
 * any stock Windows 10 machine draws with these sixteen values.
 */
struct terminal_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define TERMINAL_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct terminal_rgb campbell[TERMINAL_COLOUR_COUNT] = {
    TERMINAL_RGB(0x0CU, 0x0CU, 0x0CU),   /* black          */
    TERMINAL_RGB(0x00U, 0x37U, 0xDAU),   /* blue           */
    TERMINAL_RGB(0x13U, 0xA1U, 0x0EU),   /* green          */
    TERMINAL_RGB(0x3AU, 0x96U, 0xDDU),   /* cyan           */
    TERMINAL_RGB(0xC5U, 0x0FU, 0x1FU),   /* red            */
    TERMINAL_RGB(0x88U, 0x17U, 0x98U),   /* magenta        */
    TERMINAL_RGB(0xC1U, 0x9CU, 0x00U),   /* yellow         */
    TERMINAL_RGB(0xCCU, 0xCCU, 0xCCU),   /* white          */
    TERMINAL_RGB(0x76U, 0x76U, 0x76U),   /* bright black   */
    TERMINAL_RGB(0x3BU, 0x78U, 0xFFU),   /* bright blue    */
    TERMINAL_RGB(0x16U, 0xC6U, 0x0CU),   /* bright green   */
    TERMINAL_RGB(0x61U, 0xD6U, 0xD6U),   /* bright cyan    */
    TERMINAL_RGB(0xE7U, 0x48U, 0x56U),   /* bright red     */
    TERMINAL_RGB(0xB4U, 0x00U, 0x9EU),   /* bright magenta */
    TERMINAL_RGB(0xF9U, 0xF1U, 0xA5U),   /* bright yellow  */
    TERMINAL_RGB(0xF2U, 0xF2U, 0xF2U)    /* bright white   */
};

/* The chrome, which is not console colour and not app theme: a Windows 10
 * title bar with the accent option off is white with black text, whatever
 * else the machine is set to. */
static const struct terminal_rgb caption_active = TERMINAL_RGB(0xFFU, 0xFFU,
    0xFFU);
static const struct terminal_rgb caption_inactive = TERMINAL_RGB(0xFFU, 0xFFU,
    0xFFU);
static const struct terminal_rgb caption_text_active = TERMINAL_RGB(0x00U,
    0x00U, 0x00U);
static const struct terminal_rgb caption_text_inactive = TERMINAL_RGB(0x99U,
    0x99U, 0x99U);
static const struct terminal_rgb border_active = TERMINAL_RGB(0x00U, 0x78U,
    0xD7U);
static const struct terminal_rgb border_inactive = TERMINAL_RGB(0x9BU, 0x9BU,
    0x9BU);
static const struct terminal_rgb accent_caption = TERMINAL_RGB(0x00U, 0x78U,
    0xD7U);
/* The close button turns red under the pointer and the other two grey; the
 * red is Windows' own, not the console palette's. */
/*
 * The scrollbar is NOT console colour.  Windows 10's conhost still draws the
 * classic light-grey Win32 scrollbar down the side of a black console, arrow
 * buttons and all, and that mismatch is one of the most recognisable things
 * about a Command Prompt window.  A dark scrollbar looks tidier and is
 * wrong.
 */
static const struct terminal_rgb scrollbar_track = TERMINAL_RGB(0xF0U, 0xF0U,
    0xF0U);
static const struct terminal_rgb scrollbar_thumb = TERMINAL_RGB(0xCDU, 0xCDU,
    0xCDU);
static const struct terminal_rgb scrollbar_arrow = TERMINAL_RGB(0x60U, 0x60U,
    0x60U);

/* ================================================================== STATE */

struct terminal_glyph {
    char code;
    uint8_t foreground;
    uint8_t background;
};

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool focused = true;
static bool accent_titlebar;
static char title[TERMINAL_TITLE_BYTES] = "phip";
static struct terminal_glyph screen[TERMINAL_ROWS][TERMINAL_COLUMNS];
static uint32_t cursor_row;
static uint32_t cursor_column;
static uint8_t pen_foreground = TERMINAL_WHITE;
static uint8_t pen_background = TERMINAL_BLACK;
static bool cursor_lit = true;
static uint64_t blink_started_ns;
static const char *self_test_failure = "terminal self-test has not run";

const char *terminal_status_string(enum terminal_status status)
{
    switch (status) {
    case TERMINAL_STATUS_OK:
        return "ok";
    case TERMINAL_STATUS_NULL_ARGUMENT:
        return "null argument";
    case TERMINAL_STATUS_NOT_INITIALIZED:
        return "terminal not initialized";
    case TERMINAL_STATUS_ALREADY_INITIALIZED:
        return "terminal already initialized";
    case TERMINAL_STATUS_UNSUPPORTED_GEOMETRY:
        return "terminal geometry is unsupported";
    case TERMINAL_STATUS_SURFACE_FAILURE:
        return "terminal surface refused a pixel";
    default:
        return "unknown terminal status";
    }
}

/* ================================================================ DRAWING */

static uint32_t pack_rgb(struct terminal_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return ((uint32_t)colour.red << format.red_position) |
        ((uint32_t)colour.green << format.green_position) |
        ((uint32_t)colour.blue << format.blue_position);
}

static uint8_t channel_at(uint32_t pixel, uint8_t shift)
{
    return (uint8_t)((pixel >> shift) & 0xFFU);
}

static uint32_t mix(uint32_t under, uint32_t over, uint32_t alpha)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t inverse = 255U - alpha;
    const uint32_t red = ((uint32_t)channel_at(over, format.red_position) *
        alpha + (uint32_t)channel_at(under, format.red_position) * inverse +
        127U) / 255U;
    const uint32_t green = ((uint32_t)channel_at(over,
        format.green_position) * alpha +
        (uint32_t)channel_at(under, format.green_position) * inverse +
        127U) / 255U;
    const uint32_t blue = ((uint32_t)channel_at(over, format.blue_position) *
        alpha + (uint32_t)channel_at(under, format.blue_position) * inverse +
        127U) / 255U;

    return (red << format.red_position) | (green << format.green_position) |
        (blue << format.blue_position);
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

static enum terminal_status fill(struct ui_rect area, struct ui_rect damage,
    uint32_t colour)
{
    const struct ui_rect clipped = intersect(area, damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    colour) != SURFACE_STATUS_OK) {
                return TERMINAL_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TERMINAL_STATUS_OK;
}

/* --- geometry --- */

static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){
        window_rect.x + TERMINAL_BORDER,
        window_rect.y + TERMINAL_BORDER,
        window_rect.width - TERMINAL_BORDER * 2U,
        TERMINAL_CAPTION_HEIGHT
    };
}

static struct ui_rect caption_button_rect(uint32_t index)
{
    const struct ui_rect caption = caption_rect();
    const uint32_t from_right = 3U - index;   /* 0 minimise, 1 maximise, 2 close */

    return (struct ui_rect){
        caption.x + caption.width - from_right * TERMINAL_CAPTION_BUTTON,
        caption.y, TERMINAL_CAPTION_BUTTON, caption.height
    };
}

struct ui_rect terminal_client_bounds(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){
        window_rect.x + TERMINAL_BORDER,
        caption.y + caption.height,
        window_rect.width - TERMINAL_BORDER * 2U,
        window_rect.height - TERMINAL_BORDER * 2U - TERMINAL_CAPTION_HEIGHT
    };
}

/* Terminal takes no pointer input of its own, so this is the only place it
 * needs to ask where a point is. */
static bool holds(struct ui_rect area, struct ui_point point)
{
    return point.x >= 0 && point.y >= 0 &&
        (uint32_t)point.x >= area.x &&
        (uint32_t)point.x < area.x + area.width &&
        (uint32_t)point.y >= area.y &&
        (uint32_t)point.y < area.y + area.height;
}

/*
 * A caret over the console body, an arrow over its chrome.  A terminal is
 * text and only text: everything inside the client area is selectable, so
 * the pointer says so the whole way across it rather than only over a
 * field.
 */
enum cursor_kind terminal_cursor_at(struct ui_point point)
{
    const struct ui_rect client = terminal_client_bounds();

    if (!initialized || !holds(window_rect, point)) {
        return CURSOR_NORMAL_SELECT;
    }
    if (holds(client, point)) {
        return CURSOR_TEXT_SELECT;
    }
    return CURSOR_NORMAL_SELECT;
}

struct ui_rect terminal_bounds(void)
{
    return window_rect;
}

/* The rectangle one character occupies, in screen coordinates. */
static struct ui_rect cell_rect(uint32_t row, uint32_t column)
{
    const struct ui_rect client = terminal_client_bounds();

    return (struct ui_rect){
        client.x + TERMINAL_PADDING_X + column * CONSOLE_CELL_WIDTH,
        client.y + TERMINAL_PADDING_Y + row * CONSOLE_CELL_HEIGHT,
        CONSOLE_CELL_WIDTH, CONSOLE_CELL_HEIGHT
    };
}

static enum terminal_status draw_character(uint32_t row, uint32_t column,
    struct ui_rect damage)
{
    const struct terminal_glyph glyph = screen[row][column];
    const struct ui_rect cell = cell_rect(row, column);
    const struct ui_rect clipped = intersect(cell, damage);
    const uint32_t background = pack_rgb(campbell[glyph.background]);
    const uint32_t foreground = pack_rgb(campbell[glyph.foreground]);
    const enum terminal_status status = fill(cell, damage, background);
    const uint8_t code = (uint8_t)glyph.code;

    if (status != TERMINAL_STATUS_OK || clipped.width == 0U) {
        return status;
    }
    if (code < CONSOLE_FONT_FIRST || code > CONSOLE_FONT_LAST ||
            code == ' ') {
        return TERMINAL_STATUS_OK;
    }

    const uint8_t *cell_alpha = console_font[code - CONSOLE_FONT_FIRST];

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - cell.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - cell.x + x;
            const uint8_t coverage = cell_alpha[local_y * CONSOLE_CELL_WIDTH +
                local_x];
            uint32_t under;

            if (coverage == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    mix(under, foreground, coverage)) != SURFACE_STATUS_OK) {
                return TERMINAL_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TERMINAL_STATUS_OK;
}

/*
 * The caption's own text, drawn with the console font.
 *
 * A Windows title bar is set in Segoe UI, which is proportional, and this is
 * not: the title comes out monospaced.  That is stated rather than hidden -
 * the alternative is to reach into the UI font from a module that has no
 * other reason to know about it.
 */
static enum terminal_status draw_caption_text(struct ui_rect damage,
    uint32_t left, uint32_t baseline_top, const char *text,
    struct terminal_rgb colour)
{
    const uint32_t ink = pack_rgb(colour);
    uint32_t pen = left;

    for (size_t index = 0U; text[index] != '\0'; ++index) {
        const uint8_t code = (uint8_t)text[index];

        if (code >= CONSOLE_FONT_FIRST && code <= CONSOLE_FONT_LAST &&
                code != ' ') {
            const uint8_t *cell_alpha =
                console_font[code - CONSOLE_FONT_FIRST];
            const struct ui_rect cell = { pen, baseline_top,
                CONSOLE_CELL_WIDTH, CONSOLE_CELL_HEIGHT };
            const struct ui_rect clipped = intersect(cell, damage);

            for (uint32_t y = 0U; y < clipped.height; ++y) {
                const uint32_t local_y = clipped.y - cell.y + y;

                for (uint32_t x = 0U; x < clipped.width; ++x) {
                    const uint32_t local_x = clipped.x - cell.x + x;
                    const uint8_t coverage =
                        cell_alpha[local_y * CONSOLE_CELL_WIDTH + local_x];
                    uint32_t under;

                    if (coverage == 0U) {
                        continue;
                    }
                    if (surface_read_pixel(canvas, clipped.x + x,
                            clipped.y + y, &under) != SURFACE_STATUS_OK ||
                        surface_pixel(canvas, clipped.x + x, clipped.y + y,
                            mix(under, ink, coverage)) !=
                                SURFACE_STATUS_OK) {
                        return TERMINAL_STATUS_SURFACE_FAILURE;
                    }
                }
            }
        }
        pen += CONSOLE_CELL_WIDTH;
    }
    return TERMINAL_STATUS_OK;
}

/*
 * The three caption glyphs.
 *
 * Windows draws them from Segoe MDL2 Assets: a minus, an empty square and an
 * X, all one pixel thick and all ten pixels across, which is small enough
 * that stating them as lines is exact rather than approximate.
 */
static enum terminal_status draw_caption_glyph(uint32_t index,
    struct ui_rect damage, struct terminal_rgb colour)
{
    const struct ui_rect button = caption_button_rect(index);
    const uint32_t ink = pack_rgb(colour);
    const uint32_t size = 10U;
    const uint32_t left = button.x + (button.width - size) / 2U;
    const uint32_t top = button.y + (button.height - size) / 2U;
    enum terminal_status status = TERMINAL_STATUS_OK;

    switch (index) {
    case 0U:   /* minimise: one rule across the middle */
        status = fill((struct ui_rect){ left, top + size / 2U, size, 1U },
            damage, ink);
        break;
    case 1U:   /* maximise: an empty square */
        status = fill((struct ui_rect){ left, top, size, 1U }, damage, ink);
        if (status == TERMINAL_STATUS_OK) {
            status = fill((struct ui_rect){ left, top + size - 1U, size, 1U },
                damage, ink);
        }
        if (status == TERMINAL_STATUS_OK) {
            status = fill((struct ui_rect){ left, top, 1U, size }, damage,
                ink);
        }
        if (status == TERMINAL_STATUS_OK) {
            status = fill((struct ui_rect){ left + size - 1U, top, 1U, size },
                damage, ink);
        }
        break;
    default:   /* close: two diagonals, a pixel at a time */
        for (uint32_t step = 0U; step < size && status ==
                TERMINAL_STATUS_OK; ++step) {
            status = fill((struct ui_rect){ left + step, top + step, 1U, 1U },
                damage, ink);
            if (status == TERMINAL_STATUS_OK) {
                status = fill((struct ui_rect){
                    left + step, top + size - 1U - step, 1U, 1U }, damage,
                    ink);
            }
        }
        break;
    }
    return status;
}

/* The window icon: the console's own, which is a small black square with a
 * prompt in it - the shape cmd.exe has carried since it had sixteen colours
 * to draw it with. */
static enum terminal_status draw_window_icon(struct ui_rect damage)
{
    const struct ui_rect caption = caption_rect();
    const struct ui_rect box = {
        caption.x + TERMINAL_ICON_INSET,
        caption.y + (caption.height - TERMINAL_ICON_SIZE) / 2U,
        TERMINAL_ICON_SIZE, TERMINAL_ICON_SIZE
    };
    const uint32_t ink = pack_rgb(campbell[TERMINAL_BRIGHT_WHITE]);
    enum terminal_status status = fill(box, damage,
        pack_rgb(campbell[TERMINAL_BLACK]));

    if (status != TERMINAL_STATUS_OK) {
        return status;
    }
    /* A chevron and an underscore, three pixels in. */
    for (uint32_t step = 0U; step < 3U && status == TERMINAL_STATUS_OK;
         ++step) {
        status = fill((struct ui_rect){ box.x + 3U + step, box.y + 4U + step,
            1U, 1U }, damage, ink);
        if (status == TERMINAL_STATUS_OK) {
            status = fill((struct ui_rect){ box.x + 3U + step,
                box.y + 10U - step, 1U, 1U }, damage, ink);
        }
    }
    if (status == TERMINAL_STATUS_OK) {
        status = fill((struct ui_rect){ box.x + 8U, box.y + 11U, 5U, 1U },
            damage, ink);
    }
    return status;
}

/* One of the two arrowheads, pointing up at the top and down at the foot. */
static enum terminal_status draw_scrollbar_arrow(struct ui_rect box,
    struct ui_rect damage, bool downwards)
{
    const uint32_t ink = pack_rgb(scrollbar_arrow);
    const uint32_t rows = 4U;
    const uint32_t middle = box.x + box.width / 2U;
    const uint32_t top = box.y + (box.height - rows) / 2U;
    enum terminal_status status = TERMINAL_STATUS_OK;

    for (uint32_t row = 0U; row < rows && status == TERMINAL_STATUS_OK;
         ++row) {
        const uint32_t spread = downwards ? rows - 1U - row : row;
        const uint32_t width = spread * 2U + 1U;

        status = fill((struct ui_rect){ middle - spread, top + row, width,
            1U }, damage, ink);
    }
    return status;
}

static enum terminal_status draw_scrollbar(struct ui_rect damage)
{
    const struct ui_rect client = terminal_client_bounds();
    const struct ui_rect track = {
        client.x + client.width - TERMINAL_SCROLLBAR, client.y,
        TERMINAL_SCROLLBAR, client.height
    };
    const uint32_t button = TERMINAL_SCROLLBAR;
    enum terminal_status status = fill(track, damage,
        pack_rgb(scrollbar_track));

    if (status != TERMINAL_STATUS_OK || track.height <= button * 2U) {
        return status;
    }
    /*
     * Nothing scrolls back yet, so the thumb fills the space between the two
     * arrow buttons - which is exactly what a console with less than one
     * screen of history shows.
     */
    status = fill((struct ui_rect){ track.x, track.y + button, track.width,
        track.height - button * 2U }, damage, pack_rgb(scrollbar_thumb));
    if (status == TERMINAL_STATUS_OK) {
        status = draw_scrollbar_arrow((struct ui_rect){ track.x, track.y,
            track.width, button }, damage, false);
    }
    if (status == TERMINAL_STATUS_OK) {
        status = draw_scrollbar_arrow((struct ui_rect){ track.x,
            track.y + track.height - button, track.width, button }, damage,
            true);
    }
    return status;
}

static enum terminal_status draw_cursor(struct ui_rect damage)
{
    const struct ui_rect cell = cell_rect(cursor_row, cursor_column);
    const uint32_t height = CONSOLE_CELL_HEIGHT / TERMINAL_CURSOR_FRACTION;

    if (!cursor_lit || !focused) {
        return TERMINAL_STATUS_OK;
    }
    return fill((struct ui_rect){ cell.x, cell.y + cell.height - height,
        cell.width, height }, damage,
        pack_rgb(campbell[pen_foreground]));
}

enum terminal_status terminal_draw(struct ui_rect damage)
{
    const struct ui_rect client = terminal_client_bounds();
    const struct ui_rect caption = caption_rect();
    const struct terminal_rgb cap = accent_titlebar ? accent_caption :
        (focused ? caption_active : caption_inactive);
    const struct terminal_rgb ink = accent_titlebar ?
        campbell[TERMINAL_BRIGHT_WHITE] :
        (focused ? caption_text_active : caption_text_inactive);
    enum terminal_status status;

    if (!initialized) {
        return TERMINAL_STATUS_NOT_INITIALIZED;
    }
    /* The border first, as the whole window, then everything over it: one
     * pixel is left showing on every side, which is exactly what Windows 10
     * leaves. */
    status = fill(window_rect, damage,
        pack_rgb(focused ? border_active : border_inactive));
    if (status == TERMINAL_STATUS_OK) {
        status = fill(caption, damage, pack_rgb(cap));
    }
    if (status == TERMINAL_STATUS_OK) {
        status = fill(client, damage, pack_rgb(campbell[TERMINAL_BLACK]));
    }
    if (status == TERMINAL_STATUS_OK) {
        status = draw_window_icon(damage);
    }
    if (status == TERMINAL_STATUS_OK) {
        status = draw_caption_text(damage, caption.x + TERMINAL_TITLE_INSET,
            caption.y + (caption.height - CONSOLE_CELL_HEIGHT) / 2U, title,
            ink);
    }
    for (uint32_t index = 0U; index < 3U && status == TERMINAL_STATUS_OK;
         ++index) {
        status = draw_caption_glyph(index, damage, ink);
    }
    for (uint32_t row = 0U; row < TERMINAL_ROWS && status ==
            TERMINAL_STATUS_OK; ++row) {
        const struct ui_rect line = {
            client.x, cell_rect(row, 0U).y,
            client.width, CONSOLE_CELL_HEIGHT
        };

        if (line.y + line.height > client.y + client.height) {
            break;
        }
        if (intersect(line, damage).width == 0U) {
            continue;
        }
        for (uint32_t column = 0U; column < TERMINAL_COLUMNS && status ==
                TERMINAL_STATUS_OK; ++column) {
            if (cell_rect(row, column).x + CONSOLE_CELL_WIDTH >
                    client.x + client.width - TERMINAL_SCROLLBAR) {
                break;
            }
            status = draw_character(row, column, damage);
        }
    }
    if (status == TERMINAL_STATUS_OK) {
        status = draw_cursor(damage);
    }
    if (status == TERMINAL_STATUS_OK) {
        status = draw_scrollbar(damage);
    }
    return status;
}

/* ================================================================ WRITING */

static void clear_row(uint32_t row)
{
    for (uint32_t column = 0U; column < TERMINAL_COLUMNS; ++column) {
        screen[row][column] = (struct terminal_glyph){ ' ', pen_foreground,
            pen_background };
    }
}

static void scroll_up(void)
{
    for (uint32_t row = 1U; row < TERMINAL_ROWS; ++row) {
        for (uint32_t column = 0U; column < TERMINAL_COLUMNS; ++column) {
            screen[row - 1U][column] = screen[row][column];
        }
    }
    clear_row(TERMINAL_ROWS - 1U);
}

static void newline(void)
{
    cursor_column = 0U;
    if (cursor_row + 1U < TERMINAL_ROWS) {
        cursor_row += 1U;
    } else {
        scroll_up();
    }
}

enum terminal_status terminal_write(const char *text)
{
    if (text == NULL) {
        return TERMINAL_STATUS_NULL_ARGUMENT;
    }
    for (size_t index = 0U; text[index] != '\0'; ++index) {
        const char code = text[index];

        switch (code) {
        case '\n':
            newline();
            break;
        case '\r':
            cursor_column = 0U;
            break;
        case '\b':
            if (cursor_column > 0U) {
                cursor_column -= 1U;
                screen[cursor_row][cursor_column] = (struct terminal_glyph){
                    ' ', pen_foreground, pen_background };
            }
            break;
        case '\t':
            /* Consoles tab to the next multiple of eight, not by eight. */
            do {
                if (cursor_column + 1U >= TERMINAL_COLUMNS) {
                    newline();
                    break;
                }
                cursor_column += 1U;
            } while (cursor_column % 8U != 0U);
            break;
        default:
            if (cursor_column >= TERMINAL_COLUMNS) {
                newline();
            }
            screen[cursor_row][cursor_column] = (struct terminal_glyph){
                code, pen_foreground, pen_background };
            cursor_column += 1U;
            break;
        }
    }
    return TERMINAL_STATUS_OK;
}

enum terminal_status terminal_clear(void)
{
    for (uint32_t row = 0U; row < TERMINAL_ROWS; ++row) {
        clear_row(row);
    }
    cursor_row = 0U;
    cursor_column = 0U;
    return TERMINAL_STATUS_OK;
}

enum terminal_status terminal_set_colour(enum terminal_colour foreground,
    enum terminal_colour background)
{
    if ((size_t)foreground >= TERMINAL_COLOUR_COUNT ||
            (size_t)background >= TERMINAL_COLOUR_COUNT) {
        return TERMINAL_STATUS_UNSUPPORTED_GEOMETRY;
    }
    pen_foreground = (uint8_t)foreground;
    pen_background = (uint8_t)background;
    return TERMINAL_STATUS_OK;
}

/* ============================================================== LIFECYCLE */

enum terminal_status terminal_set_frame(struct ui_rect frame)
{
    const uint32_t least_width = TERMINAL_BORDER * 2U +
        TERMINAL_CAPTION_BUTTON * 3U + TERMINAL_SCROLLBAR;
    const uint32_t least_height = TERMINAL_BORDER * 2U +
        TERMINAL_CAPTION_HEIGHT + TERMINAL_PADDING_Y * 2U +
        CONSOLE_CELL_HEIGHT;

    if (frame.width < least_width || frame.height < least_height) {
        return TERMINAL_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return TERMINAL_STATUS_OK;
}

enum terminal_status terminal_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum terminal_status status;

    if (target == NULL) {
        return TERMINAL_STATUS_NULL_ARGUMENT;
    }
    status = terminal_set_frame(frame);
    if (status != TERMINAL_STATUS_OK) {
        return status;
    }
    canvas = target;
    (void)terminal_clear();
    blink_started_ns = clock_monotonic_ns();
    cursor_lit = true;
    initialized = true;
    return TERMINAL_STATUS_OK;
}

enum terminal_status terminal_set_title(const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        return TERMINAL_STATUS_NULL_ARGUMENT;
    }
    while (index + 1U < TERMINAL_TITLE_BYTES && text[index] != '\0') {
        title[index] = text[index];
        ++index;
    }
    title[index] = '\0';
    return TERMINAL_STATUS_OK;
}

enum terminal_status terminal_set_focus(bool active)
{
    focused = active;
    return TERMINAL_STATUS_OK;
}

enum terminal_status terminal_set_accent_titlebar(bool accented)
{
    accent_titlebar = accented;
    return TERMINAL_STATUS_OK;
}

bool terminal_blink(struct ui_rect *damage)
{
    const uint64_t now = clock_monotonic_ns();
    const uint64_t elapsed = now >= blink_started_ns ?
        now - blink_started_ns : 0U;
    const bool lit = (elapsed / TERMINAL_BLINK_NS) % 2U == 0U;

    if (damage == NULL || !initialized || lit == cursor_lit) {
        return false;
    }
    cursor_lit = lit;
    *damage = cell_rect(cursor_row, cursor_column);
    return true;
}

/* ============================================================== SELF TEST */

bool terminal_self_test(void)
{
    const uint32_t saved_row = cursor_row;
    const uint32_t saved_column = cursor_column;

    /* Campbell's black is the one value everything else is drawn against,
     * and it is 0x0C0C0C rather than 0x000000 - a console that draws pure
     * black is a console using the pre-2017 palette. */
    if (campbell[TERMINAL_BLACK].red != 0x0CU ||
            campbell[TERMINAL_BLACK].green != 0x0CU ||
            campbell[TERMINAL_BLACK].blue != 0x0CU) {
        self_test_failure = "the console black is not Campbell's";
        return false;
    }
    if (campbell[TERMINAL_WHITE].red != 0xCCU) {
        self_test_failure = "the console white is not Campbell's";
        return false;
    }
    /* Every printable character has a cell, and the space is blank. */
    for (uint32_t code = CONSOLE_FONT_FIRST; code <= CONSOLE_FONT_LAST;
         ++code) {
        uint32_t lit = 0U;

        for (uint32_t index = 0U; index < CONSOLE_CELL_WIDTH *
                CONSOLE_CELL_HEIGHT; ++index) {
            lit += console_font[code - CONSOLE_FONT_FIRST][index] != 0U ?
                1U : 0U;
        }
        if (code == ' ' && lit != 0U) {
            self_test_failure = "the console font's space has ink in it";
            return false;
        }
        if (code != ' ' && lit == 0U) {
            self_test_failure = "the console font has an empty cell";
            return false;
        }
    }

    /* Writing past the last column wraps, and past the last row scrolls. */
    cursor_row = TERMINAL_ROWS - 1U;
    cursor_column = TERMINAL_COLUMNS - 1U;
    (void)terminal_write("ab");
    if (cursor_row != TERMINAL_ROWS - 1U || cursor_column != 1U) {
        self_test_failure = "the console did not wrap and scroll at the end";
        return false;
    }
    /* A tab stops at the next multiple of eight rather than adding eight. */
    cursor_row = 0U;
    cursor_column = 3U;
    (void)terminal_write("\t");
    if (cursor_column != 8U) {
        self_test_failure = "the console tab is not to the next stop";
        return false;
    }
    (void)terminal_clear();
    cursor_row = saved_row;
    cursor_column = saved_column;
    self_test_failure = "";
    return true;
}

const char *terminal_self_test_failure(void)
{
    return self_test_failure;
}
