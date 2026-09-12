/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * The pointer.
 *
 * Windows 10's Control Panel lists fifteen system cursors under one scheme,
 * and this draws thirteen of them - the two this file leaves out are named
 * below, and are left out rather than guessed at.  Two are ANIMATED: BUSY
 * is a bare ring that spins while the whole system is unresponsive, and
 * WORKING is the ordinary arrow with the same ring badged at its foot while
 * a task runs in the background and the pointer still works.  Windows
 * tells the two apart by whether the arrow is still there; so does this.
 *
 * NOT drawn: two marks in the reference image this was built from - a hand
 * with a small person badge and a hand with a small pin badge - are not
 * part of Windows' own pointer scheme (Control Panel > Mouse > Pointers has
 * no such names), and nothing else in the reference explains what either
 * would mean.  Inventing behaviour for them would be exactly the kind of
 * fabrication the rest of this shell has tried not to do, so they are
 * skipped rather than guessed.
 *
 * WHAT THIS IS NOT: the platform's own cursor - see UI_CURSOR_WIDTH etc. in
 * phipia/ui.h - is one fixed 18x25 bitmap with one fixed hotspot, and there
 * is still no shape-switching hook in it.  ONE hook is the whole of what is
 * missing: something that takes a cursor_kind and changes what the
 * platform's pointer shows.  That is patch work in Phipia's own ui.c/ui.h,
 * the same way the taskbar itself is mounted, and not a change to this
 * file.
 *
 * Everything on this side of that hook is done.  Eight windows answer what
 * the pointer means over them, and a resolver asks them front to back and
 * sets the kind - see cursor_over() in tools/preview/main.c, which is
 * thirty lines and is carried in docs/INTEGRATION.md as the template.  A
 * compositor with the hook has to write that resolver and call
 * cursor_set_kind(); a compositor without it gets the right answer
 * computed and nowhere to put it.
 *
 * Windows draws every cursor white-filled with a black one-pixel outline,
 * for contrast against anything under it; this keeps that, and keeps the
 * ring's blue - PENDING VERIFICATION, read off the reference image rather
 * than a Microsoft source - to the two cursors that are actually animated.
 *
 * WHAT CHOOSES: not this file.  A cursor is one mark that answers "what
 * happens if I press here", so the thing that knows the answer is the
 * window under the pointer, and it is the window that says.  Eight of them
 * do: explorer_cursor_at() and taskmgr_cursor_at() return a resize at
 * their edges, paint_cursor_at() a crosshair over the sheet,
 * terminal_cursor_at() a caret across the console, notes_cursor_at() a
 * caret over the pad, settings_cursor_at() and taskbar_cursor_at() a caret
 * in their search boxes.  Editor, Camera and the dialog answer nothing,
 * because their answer is the arrow everywhere and a function that only
 * ever returns the default is not worth having.
 *
 * This module owns the SHAPES and the animation; it owns no policy about
 * when any of them is right, and a caller that never asks a window will
 * get an arrow forever, correctly.
 *
 * BUSY and WORKING are the exception, and stay the caller's to set.
 * Neither depends on where the pointer is - they say the machine is
 * thinking, which only the code doing the thinking knows.  No window
 * returns them, and none should.
 */
#ifndef PHIPIA_CURSOR_H
#define PHIPIA_CURSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/ui.h>

/* Every cursor is drawn on this square, which is what a Windows .cur file
 * at 100% scale actually is - most of it transparent, the mark sitting
 * inside it wherever its own silhouette needs. */
#define CURSOR_CANVAS 32U

enum cursor_status {
    CURSOR_STATUS_OK = 0,
    CURSOR_STATUS_NULL_ARGUMENT,
    CURSOR_STATUS_NOT_INITIALIZED,
    CURSOR_STATUS_SURFACE_FAILURE
};

/* Windows' own names, in the order its Pointers tab lists them. */
enum cursor_kind {
    CURSOR_NORMAL_SELECT = 0,
    CURSOR_HELP_SELECT,
    CURSOR_WORKING_IN_BACKGROUND,
    CURSOR_BUSY,
    CURSOR_PRECISION_SELECT,
    CURSOR_TEXT_SELECT,
    CURSOR_HANDWRITING,
    CURSOR_UNAVAILABLE,
    CURSOR_VERTICAL_RESIZE,
    CURSOR_HORIZONTAL_RESIZE,
    CURSOR_DIAGONAL_RESIZE_1,   /* north-west to south-east */
    CURSOR_DIAGONAL_RESIZE_2,   /* north-east to south-west */
    CURSOR_MOVE,
    CURSOR_ALTERNATE_SELECT,
    CURSOR_LINK_SELECT,
    CURSOR_KIND_COUNT
};

const char *cursor_status_string(enum cursor_status status);

enum cursor_status cursor_initialize(struct surface *canvas);

/* Where a cursor's canvas has to land so its HOTSPOT - the one pixel that
 * is actually "where the pointer is" - sits on the given screen point. */
struct ui_rect cursor_placement(enum cursor_kind kind, struct ui_point at);

enum cursor_status cursor_set_kind(enum cursor_kind kind);
enum cursor_kind cursor_get_kind(void);

/* Advances the busy ring's rotation.  A no-op, returning false, for every
 * kind but BUSY and WORKING - drawing either of those without calling this
 * first is a cursor that never spins. */
bool cursor_animate(struct ui_rect *damage);

enum cursor_status cursor_draw(struct ui_point at, struct ui_rect damage);

bool cursor_self_test(void);
const char *cursor_self_test_failure(void);

#endif
