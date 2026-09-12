/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * A Windows 10 console window.
 *
 * The taskbar beside this draws a shell; this draws the thing the shell
 * launches, in the same spirit: the chrome and the colours are copied rather
 * than invented, and every value that is not sourced says so where it is
 * defined.
 *
 * The colours are not a guess.  Microsoft replaced the console's palette in
 * Windows 10 1709 with a scheme it calls Campbell and published the values
 * with the ColorTool that shipped alongside it; they are what a Command
 * Prompt on any Windows 10 machine draws with, and they are reproduced
 * exactly in TERMINAL PALETTE below.
 *
 * The font is not Consolas, which cannot be redistributed.  It is DejaVu
 * Sans Mono rasterized offline into the console's own cell.
 */
#ifndef PHIPIA_TERMINAL_H
#define PHIPIA_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/cursor.h>
#include <phipia/ui.h>

/* A console is measured in characters, not pixels; these are cmd.exe's. */
#define TERMINAL_COLUMNS 80U
#define TERMINAL_ROWS 25U
/* How much history the buffer keeps above the visible rows.  cmd.exe keeps
 * 300 lines by default; this keeps fewer because a kernel pays for them in
 * static memory and nothing here scrolls back yet. */
#define TERMINAL_SCROLLBACK 64U
#define TERMINAL_TITLE_BYTES 64U

enum terminal_status {
    TERMINAL_STATUS_OK = 0,
    TERMINAL_STATUS_NULL_ARGUMENT,
    TERMINAL_STATUS_NOT_INITIALIZED,
    TERMINAL_STATUS_ALREADY_INITIALIZED,
    TERMINAL_STATUS_UNSUPPORTED_GEOMETRY,
    TERMINAL_STATUS_SURFACE_FAILURE
};

/*
 * The sixteen console colours, in the order the console numbers them.  A
 * caller says GREEN and gets Campbell's green.
 */
enum terminal_colour {
    TERMINAL_BLACK = 0,
    TERMINAL_BLUE,
    TERMINAL_GREEN,
    TERMINAL_CYAN,
    TERMINAL_RED,
    TERMINAL_MAGENTA,
    TERMINAL_YELLOW,
    TERMINAL_WHITE,
    TERMINAL_BRIGHT_BLACK,
    TERMINAL_BRIGHT_BLUE,
    TERMINAL_BRIGHT_GREEN,
    TERMINAL_BRIGHT_CYAN,
    TERMINAL_BRIGHT_RED,
    TERMINAL_BRIGHT_MAGENTA,
    TERMINAL_BRIGHT_YELLOW,
    TERMINAL_BRIGHT_WHITE,
    TERMINAL_COLOUR_COUNT
};

const char *terminal_status_string(enum terminal_status status);

/*
 * Place the window.  The rectangle is the WHOLE window - border, title bar
 * and client area - so a compositor positions it the way it positions any
 * other window.  Refuses a rectangle too small to hold the chrome and one
 * row of text.
 */
enum terminal_status terminal_initialize(struct surface *canvas,
    struct ui_rect frame);
enum terminal_status terminal_set_frame(struct ui_rect frame);
struct ui_rect terminal_bounds(void);
/* The client area alone, which is what a caller sizing its output wants. */
struct ui_rect terminal_client_bounds(void);

/* A caret over the console body, an arrow over its chrome.  See
 * phipia/cursor.h for who asks. */
enum cursor_kind terminal_cursor_at(struct ui_point point);

enum terminal_status terminal_set_title(const char *title);
/*
 * Whether this is the window in front.  Windows 10 fades an inactive
 * window's title text and drops its border to grey, and a copy that draws
 * every window as active looks like a screenshot rather than a desktop.
 */
enum terminal_status terminal_set_focus(bool focused);
/*
 * Windows 10 colours a title bar only if "Show accent color on title bars"
 * is on; off - the default - it is white with black text whatever the app
 * theme is, which is why a Command Prompt on a dark Windows 10 desktop has a
 * white cap on a black window.
 */
enum terminal_status terminal_set_accent_titlebar(bool accented);

enum terminal_status terminal_set_colour(enum terminal_colour foreground,
    enum terminal_colour background);
/* Writes text, interpreting \n, \r, \t and \b.  Scrolls at the last row. */
enum terminal_status terminal_write(const char *text);
enum terminal_status terminal_clear(void);

enum terminal_status terminal_draw(struct ui_rect damage);
/*
 * Advance the cursor's blink to the monotonic clock.  Returns true when the
 * cursor changed state, with the rectangle that has to be repainted - which
 * is one character cell, not the window.
 */
bool terminal_blink(struct ui_rect *damage);

bool terminal_self_test(void);
const char *terminal_self_test_failure(void);

#endif
