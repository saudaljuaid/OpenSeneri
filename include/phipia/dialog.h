/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * A message dialog.
 *
 * Windows 10 shows two of these and this draws the shape they share: a
 * caption with a close button and nothing else, a white body with an icon at
 * the left and one or two lines of text beside it, and a grey command strip
 * with the buttons right-aligned along it.  The metrics are Windows'
 * arrangement at Windows' proportions; the palette, the typeface and the
 * button treatment are this shell's own, so it reads as part of Phipia
 * rather than as a screenshot of something else.
 *
 * The dialog owns NO text of its own.  What it says, which icon it shows and
 * which buttons it offers all arrive through dialog_open(), because a module
 * with no failures behind it has no business inventing one to report.
 *
 * It is modal in the only sense this shell can enforce: while it is open it
 * takes the pointer and the keyboard, and dialog_is_open() tells a compositor
 * to stop handing events to whatever is underneath.
 */

#ifndef PHIPIA_DIALOG_H
#define PHIPIA_DIALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/ui.h>

enum dialog_status {
    DIALOG_STATUS_OK = 0,
    DIALOG_STATUS_NULL_ARGUMENT,
    DIALOG_STATUS_NOT_INITIALIZED,
    DIALOG_STATUS_BAD_INDEX,
    DIALOG_STATUS_UNSUPPORTED_GEOMETRY,
    DIALOG_STATUS_SURFACE_FAILURE
};

/*
 * The mark beside the message.
 *
 * Two, because two were drawn: the red disc with the cross in it, and the
 * yellow triangle with the bang.  Windows has an information mark and a
 * question mark as well; this does not draw a mark it does not have, so a
 * caller wanting one of those asks for DIALOG_ICON_NONE and the text moves
 * left to fill the space.
 */
enum dialog_icon {
    DIALOG_ICON_NONE = 0,
    DIALOG_ICON_ERROR,
    DIALOG_ICON_WARNING
};

/*
 * The buttons.  A dialog offers at most three of them, which is what a task
 * dialog offers before Windows starts stacking them vertically, and this
 * does not stack.
 */
#define DIALOG_MAX_BUTTONS 3U
#define DIALOG_TEXT_BYTES 128U

enum dialog_answer {
    DIALOG_ANSWER_NONE = 0,     /* still open, or closed without answering */
    DIALOG_ANSWER_BUTTON,       /* a button was pressed; ask which         */
    DIALOG_ANSWER_DISMISSED     /* the close mark, or Escape               */
};

struct dialog_request {
    char title[DIALOG_TEXT_BYTES];      /* the caption                     */
    char message[DIALOG_TEXT_BYTES];    /* the line in the body            */
    char detail[DIALOG_TEXT_BYTES];     /* a quieter second line, or empty */
    enum dialog_icon icon;
    char button[DIALOG_MAX_BUTTONS][24];
    size_t buttons;
    /*
     * Which button Return presses and which one is drawn with the accent
     * border.  Windows makes this the safe one, so a caller that wants
     * Cancel defaulted says so rather than relying on an order.
     */
    size_t defaulted;
};

enum dialog_status dialog_initialize(struct surface *canvas,
    struct ui_rect screen);
/* Where the dialog will sit, which is centred on the screen it was given. */
struct ui_rect dialog_bounds(void);
enum dialog_status dialog_set_screen(struct ui_rect screen);

/* Put one up.  Opening while one is already open replaces it. */
enum dialog_status dialog_open(const struct dialog_request *request,
    struct ui_rect *damage);
enum dialog_status dialog_close(struct ui_rect *damage);
bool dialog_is_open(void);

/*
 * What the person did, and what they did it with.  Reading the answer clears
 * it, so a caller that asks twice is told once - which is what stops one
 * press being acted on in two frames.
 */
enum dialog_answer dialog_take_answer(size_t *button_out);

enum dialog_status dialog_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum dialog_status dialog_pointer_press(struct ui_point point,
    struct ui_rect *damage);
enum dialog_status dialog_key_escape(struct ui_rect *damage);
enum dialog_status dialog_key_return(struct ui_rect *damage);

enum dialog_status dialog_draw(struct ui_rect damage);

bool dialog_self_test(void);
const char *dialog_self_test_failure(void);
const char *dialog_status_string(enum dialog_status status);

#endif /* PHIPIA_DIALOG_H */
