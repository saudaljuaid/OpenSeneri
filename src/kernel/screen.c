/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/font.h>
#include <phipia/framebuffer.h>
#include <phipia/screen.h>
#include <phipia/surface.h>

/*
 * Bitmap text rendered through a cached surface. Scrolling stays in write-back
 * memory, while presentation checks still read the device framebuffer.
 * Unknown bytes use a replacement glyph so console output can continue.
 */

/* What an uncovered byte is drawn as. Present in ASCII, so always available. */
#define REPLACEMENT_CHARACTER '?'

/*
 * Black on white is the deliberate classic Phipia console contract. It keeps
 * the shell legible while matching Phipia's one-bit computer-era chrome.
 */
#define SCREEN_BACKGROUND_RED UINT8_C(0xFF)
#define SCREEN_BACKGROUND_GREEN UINT8_C(0xFF)
#define SCREEN_BACKGROUND_BLUE UINT8_C(0xFF)

#define SCREEN_FOREGROUND_RED UINT8_C(0x00)
#define SCREEN_FOREGROUND_GREEN UINT8_C(0x00)
#define SCREEN_FOREGROUND_BLUE UINT8_C(0x00)

static struct screen_state state;
static uint32_t background_pixel;
static uint32_t foreground_pixel;
static struct surface back_buffer;
static char cells[SCREEN_MAX_CELLS];

struct screen_image_overlay {
    const uint32_t *pixels;
    const uint8_t *alpha;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t x;
    uint32_t row;
    uint32_t width;
    uint32_t height;
    bool active;
};

static struct screen_image_overlay image_overlay;

/*
 * The row buffer and pixel tile a glyph is copied into are local, never
 * file-scope scratch. Their combined maximum is 1,056 bytes on a 16 KiB stack,
 * and keeping them local means a thread switched out mid-glyph cannot hand the
 * next caller half of somebody else's character.
 *
 * The size is the header's bound rather than the font that happens to be built
 * in, so a taller font cannot silently overrun it. src/rust/font.rs refuses
 * anything taller, and the assertion below keeps the two numbers equal.
 */
_Static_assert(
    FONT_MAX_CELL_HEIGHT == 32U,
    "the row buffer bound no longer matches MAX_HEIGHT in src/rust/font.rs"
);
_Static_assert(
    FONT_MAX_CELL_WIDTH == 8U,
    "the cell width bound no longer matches MAX_WIDTH in src/rust/font.rs"
);

static uint32_t font_first;
static uint32_t font_count;

static bool font_covers(uint32_t code)
{
    return code >= font_first && code < font_first + font_count;
}

/*
 * Draw one glyph with its top-left corner at a cell. Every requested pixel of
 * the cell is written, lit or not, so a character replaces what was under it
 * rather than being drawn over it. Region redraws pass their exact clip: an
 * intersecting edge cell must never leak into another compositor layer.
 */
static enum screen_status paint_cell(
    uint32_t column,
    uint32_t row,
    char character,
    const struct surface_rect *clip
)
{
    uint8_t glyph_rows[FONT_MAX_CELL_HEIGHT];
    uint32_t pixels[FONT_MAX_CELL_WIDTH * FONT_MAX_CELL_HEIGHT];
    uint32_t code = (uint32_t)(unsigned char)character;

    if (!font_covers(code)) {
        code = (uint32_t)REPLACEMENT_CHARACTER;
    }

    if (phipia_font_glyph(code, glyph_rows, sizeof(glyph_rows)) !=
        FONT_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    const uint32_t origin_x = state.viewport.x + column * state.cell_width;
    const uint32_t origin_y = state.viewport.y + row * state.cell_height;
    uint32_t destination_x = origin_x;
    uint32_t destination_y = origin_y;
    uint32_t source_x = 0U;
    uint32_t source_y = 0U;
    uint32_t copy_width = state.cell_width;
    uint32_t copy_height = state.cell_height;

    for (uint32_t y = 0U; y < state.cell_height; ++y) {
        const uint8_t bits = glyph_rows[y];

        for (uint32_t x = 0U; x < state.cell_width; ++x) {
            const bool lit = (bits & (uint8_t)(0x80U >> x)) != 0U;
            pixels[y * state.cell_width + x] =
                lit ? foreground_pixel : background_pixel;
        }
    }

    if (clip != NULL) {
        const uint32_t cell_right = origin_x + state.cell_width;
        const uint32_t cell_bottom = origin_y + state.cell_height;
        const uint32_t clip_right = clip->x + clip->width;
        const uint32_t clip_bottom = clip->y + clip->height;

        if (origin_x >= clip_right || clip->x >= cell_right ||
                origin_y >= clip_bottom || clip->y >= cell_bottom) {
            return SCREEN_STATUS_OK;
        }
        destination_x = origin_x > clip->x ? origin_x : clip->x;
        destination_y = origin_y > clip->y ? origin_y : clip->y;
        const uint32_t destination_right = cell_right < clip_right ?
            cell_right : clip_right;
        const uint32_t destination_bottom = cell_bottom < clip_bottom ?
            cell_bottom : clip_bottom;

        source_x = destination_x - origin_x;
        source_y = destination_y - origin_y;
        copy_width = destination_right - destination_x;
        copy_height = destination_bottom - destination_y;
    }

    if (surface_blit(&back_buffer, destination_x, destination_y,
            &pixels[source_y * state.cell_width + source_x],
            copy_width, copy_height,
            state.cell_width * SURFACE_BYTES_PER_PIXEL) != SURFACE_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    return SCREEN_STATUS_OK;
}

static enum screen_status draw_cell(
    uint32_t column,
    uint32_t row,
    char character
)
{
    enum screen_status status;

    if (!state.visible) {
        return SCREEN_STATUS_OK;
    }
    status = paint_cell(column, row, character, NULL);
    if (status != SCREEN_STATUS_OK) {
        return status;
    }
    if (!state.deferred_present &&
        surface_present(&back_buffer) != SURFACE_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }
    return SCREEN_STATUS_OK;
}

/*
 * Move everything up one line. The exposed band is one cell tall rather than
 * the remainder of the screen, because the grid is derived by division and the
 * last few pixel rows below the final cell are not part of any cell.
 */
static enum screen_status scroll_one_line(void)
{
    struct surface_rect source;
    struct surface_rect exposed;

    image_overlay.active = false;
    for (uint32_t row = 1U; row < state.rows; ++row) {
        for (uint32_t column = 0U; column < state.columns; ++column) {
            cells[(row - 1U) * SCREEN_MAX_COLUMNS + column] =
                cells[row * SCREEN_MAX_COLUMNS + column];
        }
    }
    for (uint32_t column = 0U; column < state.columns; ++column) {
        cells[(state.rows - 1U) * SCREEN_MAX_COLUMNS + column] = ' ';
    }

    if (!state.visible) {
        state.scrolls += 1U;
        return SCREEN_STATUS_OK;
    }

    source.x = state.viewport.x;
    source.y = state.viewport.y + state.cell_height;
    source.width = state.viewport.width;
    source.height = state.viewport.height - state.cell_height;
    exposed.x = state.viewport.x;
    exposed.y = state.viewport.y + state.viewport.height - state.cell_height;
    exposed.width = state.viewport.width;
    exposed.height = state.cell_height;

    if (surface_copy_rect(&back_buffer, source, state.viewport.x,
            state.viewport.y) !=
            SURFACE_STATUS_OK ||
        surface_fill_rect(&back_buffer, exposed, background_pixel) !=
            SURFACE_STATUS_OK ||
        (!state.deferred_present &&
            surface_present(&back_buffer) != SURFACE_STATUS_OK)) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    state.scrolls += 1U;
    return SCREEN_STATUS_OK;
}

static enum screen_status newline(void)
{
    state.column = 0U;

    if (state.row + 1U < state.rows) {
        state.row += 1U;
        return SCREEN_STATUS_OK;
    }

    /*
     * The cursor stays on the last row and the picture moves instead, which is
     * what makes the bottom line the live one.
     */
    return scroll_one_line();
}

/*
 * How many whole cells fit. Split out from screen_initialize because it is the
 * only arithmetic here that can be wrong without a framebuffer to be wrong on,
 * and so the only part the self-test can reach before boot has one.
 *
 * A partial cell at the right or bottom edge is not a cell. Reporting one would
 * put the console's own bounds check in disagreement with the framebuffer's.
 */
static bool grid_for(
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t cell_width,
    uint32_t cell_height,
    uint32_t *columns,
    uint32_t *rows
)
{
    if (cell_width == 0U || cell_height == 0U) {
        return false;
    }

    const uint32_t across = screen_width / cell_width;
    const uint32_t down = screen_height / cell_height;

    /*
     * One column and one row is the smallest thing that is still a console.
     * Below that the arithmetic still works and the result is not a console,
     * so it is refused here rather than discovered by a caller.
     */
    if (across == 0U || down == 0U) {
        return false;
    }

    *columns = across;
    *rows = down;
    return true;
}

enum screen_status screen_initialize(void)
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t first = 0U;
    uint32_t count = 0U;

    if (state.active) {
        return SCREEN_STATUS_ALREADY_INITIALIZED;
    }

    if (!framebuffer_is_active()) {
        return SCREEN_STATUS_NO_FRAMEBUFFER;
    }

    if (phipia_font_geometry(&width, &height, &first, &count) !=
        FONT_STATUS_OK) {
        return SCREEN_STATUS_BAD_FONT;
    }

    if (height > FONT_MAX_CELL_HEIGHT) {
        return SCREEN_STATUS_CELL_TOO_LARGE;
    }

    if (width > FONT_MAX_CELL_WIDTH) {
        return SCREEN_STATUS_CELL_TOO_LARGE;
    }

    const struct framebuffer_state framebuffer = framebuffer_get_state();
    uint32_t columns = 0U;
    uint32_t rows = 0U;

    if (!grid_for(framebuffer.width, framebuffer.height, width, height,
            &columns, &rows)) {
        return SCREEN_STATUS_NO_ROOM;
    }
    if (columns > SCREEN_MAX_COLUMNS || rows > SCREEN_MAX_ROWS) {
        return SCREEN_STATUS_CELL_CAPACITY;
    }

    font_first = first;
    font_count = count;

    /*
     * The replacement has to be one the font covers, or an uncovered byte
     * would recurse into another uncovered byte.
     */
    if (!font_covers((uint32_t)REPLACEMENT_CHARACTER)) {
        return SCREEN_STATUS_BAD_FONT;
    }

    background_pixel = framebuffer_pack(SCREEN_BACKGROUND_RED,
        SCREEN_BACKGROUND_GREEN, SCREEN_BACKGROUND_BLUE);
    foreground_pixel = framebuffer_pack(SCREEN_FOREGROUND_RED,
        SCREEN_FOREGROUND_GREEN, SCREEN_FOREGROUND_BLUE);

    if (surface_initialize(&back_buffer, framebuffer.width,
            framebuffer.height) != SURFACE_STATUS_OK) {
        return SCREEN_STATUS_SURFACE_FAILURE;
    }

    state.columns = columns;
    state.rows = rows;
    state.cell_width = width;
    state.cell_height = height;
    state.column = 0U;
    state.row = 0U;
    state.characters = 0U;
    state.scrolls = 0U;
    state.viewport = (struct surface_rect){
        0U, 0U, framebuffer.width, framebuffer.height
    };
    state.visible = true;
    state.deferred_present = false;
    state.active = true;

    for (size_t index = 0U; index < SCREEN_MAX_CELLS; ++index) {
        cells[index] = ' ';
    }

    return screen_clear();
}

enum screen_status screen_release(void)
{
    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }

    if (surface_release(&back_buffer) != SURFACE_STATUS_OK) {
        return SCREEN_STATUS_SURFACE_FAILURE;
    }

    /*
     * A released console must stop receiving mirrored output immediately. A
     * partially live cursor would let a later write reach a surface whose heap
     * storage has already been returned.
     */
    state = (struct screen_state){ 0 };
    image_overlay = (struct screen_image_overlay){ 0 };
    background_pixel = 0U;
    foreground_pixel = 0U;
    font_first = 0U;
    font_count = 0U;
    for (size_t index = 0U; index < SCREEN_MAX_CELLS; ++index) {
        cells[index] = ' ';
    }
    return SCREEN_STATUS_OK;
}

bool screen_is_active(void)
{
    return state.active;
}

enum screen_status screen_clear(void)
{
    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }

    image_overlay.active = false;
    for (uint32_t row = 0U; row < state.rows; ++row) {
        for (uint32_t column = 0U; column < state.columns; ++column) {
            cells[row * SCREEN_MAX_COLUMNS + column] = ' ';
        }
    }

    if (state.visible &&
        (surface_fill_rect(&back_buffer, state.viewport, background_pixel) !=
            SURFACE_STATUS_OK ||
         (!state.deferred_present &&
            surface_present(&back_buffer) != SURFACE_STATUS_OK))) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    state.column = 0U;
    state.row = 0U;
    return SCREEN_STATUS_OK;
}

static bool rectangle_end(
    struct surface_rect rectangle,
    uint32_t *right,
    uint32_t *bottom
)
{
    if (right == NULL || bottom == NULL || rectangle.width == 0U ||
        rectangle.height == 0U ||
        rectangle.x > UINT32_MAX - rectangle.width ||
        rectangle.y > UINT32_MAX - rectangle.height) {
        return false;
    }
    *right = rectangle.x + rectangle.width;
    *bottom = rectangle.y + rectangle.height;
    return true;
}

static bool rectangles_intersect(
    struct surface_rect left,
    struct surface_rect right
)
{
    uint32_t left_right;
    uint32_t left_bottom;
    uint32_t right_right;
    uint32_t right_bottom;

    return rectangle_end(left, &left_right, &left_bottom) &&
        rectangle_end(right, &right_right, &right_bottom) &&
        left.x < right_right && right.x < left_right &&
        left.y < right_bottom && right.y < left_bottom;
}

static struct surface_rect rectangle_intersection(
    struct surface_rect left,
    struct surface_rect right
)
{
    uint32_t left_right;
    uint32_t left_bottom;
    uint32_t right_right;
    uint32_t right_bottom;
    struct surface_rect result = { 0U, 0U, 0U, 0U };

    if (!rectangles_intersect(left, right) ||
        !rectangle_end(left, &left_right, &left_bottom) ||
        !rectangle_end(right, &right_right, &right_bottom)) {
        return result;
    }
    result.x = left.x > right.x ? left.x : right.x;
    result.y = left.y > right.y ? left.y : right.y;
    const uint32_t end_x = left_right < right_right ? left_right : right_right;
    const uint32_t end_y = left_bottom < right_bottom ? left_bottom : right_bottom;
    result.width = end_x - result.x;
    result.height = end_y - result.y;
    return result;
}

static enum screen_status paint_image_overlay(struct surface_rect clip)
{
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    struct surface_rect bounds;
    struct surface_rect clipped;

    if (!image_overlay.active || !state.visible) {
        return SCREEN_STATUS_OK;
    }
    bounds = (struct surface_rect){
        state.viewport.x + image_overlay.x,
        state.viewport.y + image_overlay.row * state.cell_height,
        image_overlay.width,
        image_overlay.height
    };
    if (!rectangles_intersect(bounds, clip)) {
        return SCREEN_STATUS_OK;
    }
    clipped = rectangle_intersection(bounds, clip);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t source_x = (uint32_t)(
                (uint64_t)(clipped.x - bounds.x + x) *
                    image_overlay.source_width / bounds.width
            );
            const uint32_t source_y = (uint32_t)(
                (uint64_t)(clipped.y - bounds.y + y) *
                    image_overlay.source_height / bounds.height
            );
            const size_t source = (size_t)source_y *
                image_overlay.source_width + source_x;
            const uint8_t opacity = image_overlay.alpha[source];
            uint32_t pixel = image_overlay.pixels[source];

            if (opacity == 0U) {
                continue;
            }
            if (opacity != UINT8_MAX) {
                const uint8_t shifts[3U] = { framebuffer.red_position,
                    framebuffer.green_position, framebuffer.blue_position };
                uint32_t under;

                if (surface_read_pixel(&back_buffer, clipped.x + x,
                        clipped.y + y, &under) != SURFACE_STATUS_OK) {
                    return SCREEN_STATUS_DRAW_FAILURE;
                }
                pixel = 0U;
                for (size_t channel = 0U; channel < 3U; ++channel) {
                    const uint8_t shift = shifts[channel];
                    const uint32_t foreground =
                        (image_overlay.pixels[source] >> shift) & 0xFFU;
                    const uint32_t background = (under >> shift) & 0xFFU;
                    uint32_t value = foreground +
                        (background * (UINT8_MAX - opacity) + 127U) /
                            UINT8_MAX;

                    if (value > UINT8_MAX) {
                        value = UINT8_MAX;
                    }
                    pixel |= value << shift;
                }
            }
            if (surface_pixel(&back_buffer, clipped.x + x, clipped.y + y,
                    pixel) != SURFACE_STATUS_OK) {
                return SCREEN_STATUS_DRAW_FAILURE;
            }
        }
    }
    return SCREEN_STATUS_OK;
}

struct surface *screen_surface(void)
{
    return state.active ? &back_buffer : NULL;
}

enum screen_status screen_redraw_region(struct surface_rect clip)
{
    uint32_t clip_right;
    uint32_t clip_bottom;
    struct surface_rect redraw;

    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }
    if (!rectangle_end(clip, &clip_right, &clip_bottom) ||
        clip_right > back_buffer.width || clip_bottom > back_buffer.height) {
        return SCREEN_STATUS_BAD_VIEWPORT;
    }
    if (!state.visible || !rectangles_intersect(clip, state.viewport)) {
        return SCREEN_STATUS_OK;
    }

    redraw = rectangle_intersection(clip, state.viewport);
    if (surface_fill_rect(&back_buffer, redraw, background_pixel) !=
        SURFACE_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    for (uint32_t row = 0U; row < state.rows; ++row) {
        for (uint32_t column = 0U; column < state.columns; ++column) {
            const struct surface_rect cell = {
                state.viewport.x + column * state.cell_width,
                state.viewport.y + row * state.cell_height,
                state.cell_width,
                state.cell_height
            };

            if (rectangles_intersect(cell, redraw) &&
                paint_cell(column, row,
                    cells[row * SCREEN_MAX_COLUMNS + column], &redraw) !=
                    SCREEN_STATUS_OK) {
                return SCREEN_STATUS_DRAW_FAILURE;
            }
        }
    }

    if (paint_image_overlay(redraw) != SCREEN_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    if (!state.deferred_present &&
        surface_present(&back_buffer) != SURFACE_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }
    return SCREEN_STATUS_OK;
}

enum screen_status screen_set_viewport(
    struct surface_rect viewport,
    bool visible
)
{
    uint32_t right;
    uint32_t bottom;
    uint32_t columns;
    uint32_t rows;
    const uint32_t old_columns = state.columns;
    const uint32_t old_rows = state.rows;
    const uint32_t old_row = state.row;

    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }
    if (!rectangle_end(viewport, &right, &bottom) ||
        right > back_buffer.width || bottom > back_buffer.height ||
        !grid_for(viewport.width, viewport.height, state.cell_width,
            state.cell_height, &columns, &rows)) {
        return SCREEN_STATUS_BAD_VIEWPORT;
    }
    if (columns > SCREEN_MAX_COLUMNS || rows > SCREEN_MAX_ROWS) {
        return SCREEN_STATUS_CELL_CAPACITY;
    }

    if (columns != old_columns || rows != old_rows) {
        image_overlay.active = false;
        const uint32_t active_rows = old_row + 1U < old_rows ?
            old_row + 1U : old_rows;
        const uint32_t first_row = active_rows > rows ? active_rows - rows : 0U;
        const uint32_t copied_rows = active_rows - first_row < rows ?
            active_rows - first_row : rows;
        const uint32_t copied_columns = old_columns < columns ?
            old_columns : columns;

        for (uint32_t row = 0U; row < copied_rows; ++row) {
            for (uint32_t column = 0U; column < copied_columns; ++column) {
                cells[row * SCREEN_MAX_COLUMNS + column] =
                    cells[(first_row + row) * SCREEN_MAX_COLUMNS + column];
            }
            for (uint32_t column = copied_columns; column < columns; ++column) {
                cells[row * SCREEN_MAX_COLUMNS + column] = ' ';
            }
        }
        for (uint32_t row = copied_rows; row < rows; ++row) {
            for (uint32_t column = 0U; column < columns; ++column) {
                cells[row * SCREEN_MAX_COLUMNS + column] = ' ';
            }
        }
        state.row = old_row >= first_row ? old_row - first_row : 0U;
        if (state.row >= rows) {
            state.row = rows - 1U;
        }
    }

    state.viewport = viewport;
    state.columns = columns;
    state.rows = rows;
    if (state.column >= columns) {
        state.column = columns - 1U;
    }
    state.visible = visible;
    return visible ? screen_redraw_region(viewport) : SCREEN_STATUS_OK;
}

enum screen_status screen_set_visible(bool visible)
{
    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }
    state.visible = visible;
    return visible ? screen_redraw_region(state.viewport) : SCREEN_STATUS_OK;
}

enum screen_status screen_set_deferred_present(bool deferred)
{
    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }
    state.deferred_present = deferred;
    return SCREEN_STATUS_OK;
}

enum screen_status screen_set_palette(
    uint8_t background_red,
    uint8_t background_green,
    uint8_t background_blue,
    uint8_t foreground_red,
    uint8_t foreground_green,
    uint8_t foreground_blue
)
{
    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }
    background_pixel = framebuffer_pack(background_red, background_green,
        background_blue);
    foreground_pixel = framebuffer_pack(foreground_red, foreground_green,
        foreground_blue);
    return SCREEN_STATUS_OK;
}

enum screen_status screen_draw_image(
    const uint32_t *pixels,
    const uint8_t *alpha,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t width,
    uint32_t height,
    uint32_t reserved_rows
)
{
    const uint32_t inset = state.cell_width * 2U;

    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }
    if (pixels == NULL || alpha == NULL || source_width == 0U ||
        source_height == 0U || width == 0U || height == 0U ||
        reserved_rows == 0U ||
        (size_t)source_width > SIZE_MAX / source_height ||
        inset > state.viewport.width || width > state.viewport.width - inset ||
        reserved_rows >= state.rows ||
        state.row > state.rows - 1U - reserved_rows ||
        reserved_rows > UINT32_MAX / state.cell_height ||
        height > reserved_rows * state.cell_height) {
        return SCREEN_STATUS_BAD_IMAGE;
    }
    image_overlay = (struct screen_image_overlay){
        .pixels = pixels,
        .alpha = alpha,
        .source_width = source_width,
        .source_height = source_height,
        .x = inset,
        .row = state.row,
        .width = width,
        .height = height,
        .active = true
    };
    for (uint32_t row = state.row; row < state.row + reserved_rows; ++row) {
        for (uint32_t column = 0U; column < state.columns; ++column) {
            cells[row * SCREEN_MAX_COLUMNS + column] = ' ';
        }
    }
    state.column = 0U;
    state.row += reserved_rows;
    if (paint_image_overlay(state.viewport) != SCREEN_STATUS_OK ||
        (!state.deferred_present &&
            surface_present(&back_buffer) != SURFACE_STATUS_OK)) {
        image_overlay.active = false;
        return SCREEN_STATUS_DRAW_FAILURE;
    }
    return SCREEN_STATUS_OK;
}

enum screen_status screen_putc(char character)
{
    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }

    if (character == '\n') {
        return newline();
    }

    /*
     * A carriage return on its own returns to the start of the line. The serial
     * console emits one before every newline, and without this the pair would
     * cost two lines instead of one.
     */
    if (character == '\r') {
        state.column = 0U;
        return SCREEN_STATUS_OK;
    }

    /*
     * Backspace moves the cursor and erases nothing, which is what a terminal
     * does: erasing is the caller's three-character sequence of back, space,
     * back. Without this the byte falls through to the drawing path, is not
     * covered by the font, and appears on screen as the replacement character -
     * so every correction a person made would leave a '?' behind.
     *
     * It stops at the first column rather than wrapping to the end of the row
     * above. A console that reverses over a line break has to remember how long
     * that line was, and nothing here does.
     */
    if (character == '\b') {
        if (state.column > 0U) {
            state.column -= 1U;
        }

        return SCREEN_STATUS_OK;
    }

    if (state.column >= state.columns) {
        const enum screen_status status = newline();

        if (status != SCREEN_STATUS_OK) {
            return status;
        }
    }

    cells[state.row * SCREEN_MAX_COLUMNS + state.column] = character;
    const enum screen_status status =
        draw_cell(state.column, state.row, character);

    if (status != SCREEN_STATUS_OK) {
        return status;
    }

    state.column += 1U;
    state.characters += 1U;
    return SCREEN_STATUS_OK;
}

enum screen_status screen_write(const char *text)
{
    if (text == NULL) {
        return SCREEN_STATUS_OK;
    }

    for (size_t index = 0; text[index] != '\0'; ++index) {
        const enum screen_status status = screen_putc(text[index]);

        if (status != SCREEN_STATUS_OK) {
            return status;
        }
    }

    return SCREEN_STATUS_OK;
}

struct screen_state screen_get_state(void)
{
    return state;
}

enum screen_status screen_verify_cell(
    uint32_t column,
    uint32_t row,
    char expected
)
{
    uint8_t glyph_rows[FONT_MAX_CELL_HEIGHT];

    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }

    if (column >= state.columns || row >= state.rows) {
        return SCREEN_STATUS_NO_ROOM;
    }

    uint32_t code = (uint32_t)(unsigned char)expected;

    if (!font_covers(code)) {
        code = (uint32_t)REPLACEMENT_CHARACTER;
    }

    if (phipia_font_glyph(code, glyph_rows, sizeof(glyph_rows)) !=
        FONT_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    const uint32_t origin_x = state.viewport.x + column * state.cell_width;
    const uint32_t origin_y = state.viewport.y + row * state.cell_height;
    const uint32_t mask = framebuffer_visible_mask();

    for (uint32_t y = 0; y < state.cell_height; ++y) {
        const uint8_t bits = glyph_rows[y];

        for (uint32_t x = 0; x < state.cell_width; ++x) {
            const bool lit = (bits & (uint8_t)(0x80U >> x)) != 0U;
            const uint32_t want = lit ? foreground_pixel : background_pixel;
            uint32_t got = 0U;

            if (framebuffer_read_pixel(origin_x + x, origin_y + y, &got) !=
                FRAMEBUFFER_STATUS_OK) {
                return SCREEN_STATUS_DRAW_FAILURE;
            }

            if ((got & mask) != (want & mask)) {
                return SCREEN_STATUS_DRAW_FAILURE;
            }
        }
    }

    return SCREEN_STATUS_OK;
}

const char *screen_status_string(enum screen_status status)
{
    static const char *const messages[] = {
        "ok",
        "no framebuffer to put text on",
        "screen console is already initialized",
        "screen console is not initialized",
        "font table is unusable for a console",
        "font cell is taller than the row buffer",
        "framebuffer has no room for a character grid",
        "screen console could not create its back buffer",
        "screen console failed to draw",
        "screen console viewport is invalid",
        "screen console cell capacity is exceeded",
        "screen console image is invalid"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)SCREEN_STATUS_BAD_IMAGE + 1U,
        "screen status messages are out of sync with enum screen_status"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown screen status";
    }

    return messages[status];
}

/*
 * What this layer must refuse, checked before boot has a framebuffer.
 *
 * Only two things here can be tested without one: the grid arithmetic, and the
 * refusal to draw before initialization. Everything else about a console is a
 * claim about pixels, and pixels are checked by prove_screen_console in
 * src/kernel/boot_proofs.c reading them back off the glass.
 */
static bool grid_is_right(void)
{
    uint32_t columns = 0U;
    uint32_t rows = 0U;

    /* The mode boot actually runs in. */
    if (!grid_for(1024U, 768U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    if (columns != 128U || rows != 48U) {
        return false;
    }

    /* The smallest mode this kernel accepts. */
    if (!grid_for(640U, 480U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    if (columns != 80U || rows != 30U) {
        return false;
    }

    /*
     * A partial cell at either edge is not a cell. 1023 pixels across an
     * 8-pixel cell is 127 whole ones and seven pixels of nothing.
     */
    if (!grid_for(1023U, 767U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    if (columns != 127U || rows != 47U) {
        return false;
    }

    /* Narrower or shorter than one cell is not a console. */
    if (grid_for(7U, 768U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    if (grid_for(1024U, 15U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    /* A cell with no extent would divide by zero. */
    if (grid_for(1024U, 768U, 0U, 16U, &columns, &rows)) {
        return false;
    }

    if (grid_for(1024U, 768U, 8U, 0U, &columns, &rows)) {
        return false;
    }

    return true;
}

static bool refusals_are_named(void)
{
    static const enum screen_status every[] = {
        SCREEN_STATUS_OK,
        SCREEN_STATUS_NO_FRAMEBUFFER,
        SCREEN_STATUS_ALREADY_INITIALIZED,
        SCREEN_STATUS_NOT_INITIALIZED,
        SCREEN_STATUS_BAD_FONT,
        SCREEN_STATUS_CELL_TOO_LARGE,
        SCREEN_STATUS_NO_ROOM,
        SCREEN_STATUS_SURFACE_FAILURE,
        SCREEN_STATUS_DRAW_FAILURE,
        SCREEN_STATUS_BAD_VIEWPORT,
        SCREEN_STATUS_CELL_CAPACITY,
        SCREEN_STATUS_BAD_IMAGE
    };

    for (size_t index = 0; index < sizeof(every) / sizeof(every[0]); ++index) {
        const char *message = screen_status_string(every[index]);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
    }

    /* A value outside the enum must be named rather than indexed. */
    return screen_status_string((enum screen_status)99) != NULL;
}

bool screen_self_test(void)
{
    if (!grid_is_right()) {
        return false;
    }

    if (!refusals_are_named()) {
        return false;
    }

    /*
     * Before initialization every entry point refuses. This runs before the
     * framebuffer is adopted, so it is the real state rather than a simulated
     * one - which also means it can only be checked once, and only here.
     */
    if (!state.active) {
        const uint32_t pixel = 0U;
        const uint8_t alpha = 0U;

        if (screen_putc('x') != SCREEN_STATUS_NOT_INITIALIZED) {
            return false;
        }

        if (screen_clear() != SCREEN_STATUS_NOT_INITIALIZED) {
            return false;
        }

        if (screen_verify_cell(0U, 0U, 'x') != SCREEN_STATUS_NOT_INITIALIZED) {
            return false;
        }

        if (screen_draw_image(&pixel, &alpha, 1U, 1U, 1U, 1U, 1U) !=
            SCREEN_STATUS_NOT_INITIALIZED) {
            return false;
        }
    }

    return true;
}
