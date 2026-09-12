/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Notes.
 *
 * Windows 10 ships Sticky Notes: a yellow pad with a coloured header strip, a
 * plus at one end and an overflow menu at the other, and a formatting bar
 * along the bottom.  This keeps that - the yellow, the strip, the shape - and
 * fixes the two things about it that are genuinely awkward.
 *
 * Sticky Notes puts every note in its own floating window, so six notes are
 * six windows to arrange, and it hides the list of them behind a separate
 * "Notes list" window.  Here the list is a pane of the same window: the notes
 * on the left, the open one on the right, with the selected one marked by an
 * accent bar the way every Windows list marks a selection.
 *
 * And its checklist is a text convention rather than a control.  The icon on
 * this one is a note with a tick on it, so the tick is real: an item is done
 * or it is not, the box is drawn either way, and a finished item is struck
 * through and faded rather than deleted.
 */
#ifndef PHIPIA_NOTES_H
#define PHIPIA_NOTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/cursor.h>
#include <phipia/ui.h>

#define NOTES_MAX_NOTES 8U
#define NOTES_MAX_LINES 16U
#define NOTES_TEXT_BYTES 64U

enum notes_status {
    NOTES_STATUS_OK = 0,
    NOTES_STATUS_NULL_ARGUMENT,
    NOTES_STATUS_NOT_INITIALIZED,
    NOTES_STATUS_BAD_INDEX,
    NOTES_STATUS_UNSUPPORTED_GEOMETRY,
    NOTES_STATUS_SURFACE_FAILURE
};

/*
 * The five colours Sticky Notes offers, which are also the only ones it
 * offers - a note is one of these or it is not a Sticky Note.
 */
enum notes_colour {
    NOTES_YELLOW = 0,
    NOTES_GREEN,
    NOTES_PINK,
    NOTES_PURPLE,
    NOTES_BLUE,
    NOTES_COLOUR_COUNT
};

/* A line of a note.  A line with `checkable` set draws a box in front of it
 * and, when `done`, strikes it through. */
struct notes_line {
    bool present;
    bool checkable;
    bool done;
    bool bold;
    bool italic;
    bool underline;
    bool strike;
    char text[NOTES_TEXT_BYTES];
};

struct notes_note {
    bool present;
    enum notes_colour colour;
    char title[NOTES_TEXT_BYTES];
    struct notes_line lines[NOTES_MAX_LINES];
};

const char *notes_status_string(enum notes_status status);

enum notes_status notes_initialize(struct surface *canvas,
    struct ui_rect frame);
enum notes_status notes_set_frame(struct ui_rect frame);
struct ui_rect notes_bounds(void);

/* A caret over the pad, an arrow over the list.  See phipia/cursor.h. */
enum cursor_kind notes_cursor_at(struct ui_point point);

enum notes_status notes_set_note(size_t index, const struct notes_note *note);
enum notes_status notes_set_line(size_t note, size_t line,
    const struct notes_line *value);
enum notes_status notes_select(size_t index);
size_t notes_selected(void);
enum notes_status notes_new(struct ui_rect *damage);
enum notes_status notes_text_input(char character, struct ui_rect *damage);
enum notes_status notes_key_backspace(struct ui_rect *damage);
enum notes_status notes_key_enter(struct ui_rect *damage);
enum notes_status notes_get_note(size_t index, struct notes_note *note);
enum notes_status notes_set_focus(bool focused);

/* Which line the pointer is over, so a hover can be drawn; -1 for none. */
enum notes_status notes_pointer_move(struct ui_point point,
    struct ui_rect *damage);
/* Clicking a check box toggles it; clicking a note in the list selects it. */
enum notes_status notes_pointer_press(struct ui_point point,
    struct ui_rect *damage);

enum notes_status notes_draw(struct ui_rect damage);

bool notes_self_test(void);
const char *notes_self_test_failure(void);

#endif
