/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_SCREEN_H
#define PHIPIA_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include <phipia/surface.h>

#define SCREEN_MAX_COLUMNS 160U
#define SCREEN_MAX_ROWS 48U
#define SCREEN_MAX_CELLS (SCREEN_MAX_COLUMNS * SCREEN_MAX_ROWS)

/*
 * Text on the framebuffer.
 *
 * Until this existed, Phipia could draw pixels and could write words, but not
 * both: the console spoke to a serial port and a VGA text buffer, and the
 * framebuffer knew nothing about characters. This is the layer between them.
 *
 * It owns no policy about what gets printed. src/kernel/console.c decides that
 * and calls in; this decides only where a character lands and what happens
 * when the screen fills.
 */

enum screen_status {
    SCREEN_STATUS_OK = 0,
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

struct screen_state {
    bool active;
    uint32_t columns;
    uint32_t rows;
    uint32_t cell_width;
    uint32_t cell_height;
    uint32_t column;      /* the cursor */
    uint32_t row;
    uint64_t characters;  /* how many have been drawn */
    uint64_t scrolls;
    struct surface_rect viewport;
    bool visible;
    bool deferred_present;
};

/*
 * Take the framebuffer and the built-in font and work out the character grid.
 * Refuses rather than truncates: a screen that cannot hold one full row of one
 * full line of text is not a console, and saying so is better than silently
 * drawing half a character.
 */
enum screen_status screen_initialize(void);

/* Give the owned surface back to the heap and stop mirroring console output. */
enum screen_status screen_release(void);

bool screen_is_active(void);

/*
 * Draw one character at the cursor and advance it. A newline moves to column
 * zero of the next row; anything the font does not cover is drawn as the
 * replacement the font does cover, because a console that refuses to print an
 * unexpected byte is a console that loses the message explaining it.
 */
enum screen_status screen_putc(char character);

enum screen_status screen_write(const char *text);

/* Clear the screen and return the cursor to the origin. */
enum screen_status screen_clear(void);

/*
 * Phipia borrows the one long-lived cached surface and constrains this
 * console to a fixed terminal client rectangle. Cells remain in a bounded
 * backing store while hidden, so reopening redraws rather than resets them.
 */
struct surface *screen_surface(void);
enum screen_status screen_set_viewport(
    struct surface_rect viewport,
    bool visible
);
enum screen_status screen_set_visible(bool visible);
enum screen_status screen_set_deferred_present(bool deferred);
enum screen_status screen_redraw_region(struct surface_rect clip);
enum screen_status screen_set_palette(
    uint8_t background_red,
    uint8_t background_green,
    uint8_t background_blue,
    uint8_t foreground_red,
    uint8_t foreground_green,
    uint8_t foreground_blue
);

/*
 * Place one bounded, alpha-backed image at the text cursor and reserve complete
 * rows below it. Pixels are packed for the installed framebuffer and already
 * precomposed over black; source storage remains caller-owned.
 */
enum screen_status screen_draw_image(
    const uint32_t *pixels,
    const uint8_t *alpha,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t width,
    uint32_t height,
    uint32_t reserved_rows
);

struct screen_state screen_get_state(void);

/*
 * Re-read one drawn character straight out of the framebuffer and compare it
 * with what the font says it should be. This is what makes the console's claim
 * checkable: everything else here is write-only, and a write-only path proves
 * nothing about what is on the glass.
 */
enum screen_status screen_verify_cell(uint32_t column, uint32_t row, char expected);

bool screen_self_test(void);
const char *screen_status_string(enum screen_status status);

#endif
