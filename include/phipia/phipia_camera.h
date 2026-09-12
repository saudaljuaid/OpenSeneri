/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Camera.
 *
 * Three bands: a dark control bar across the top, the viewfinder, and a
 * black bar along the bottom with the shutter in the middle of it.
 *
 *   TOP     a timer on the left; the photo/video segmented control in the
 *           CENTRE; the menu and the close mark on the right.  The top bar
 *           is the window's chrome - there is no separate title bar.
 *   MIDDLE  the frame, four to three, full width, letterboxed above and
 *           below in black.
 *   BOTTOM  the SHUTTER, a white disc inside a ring, dead centre; the last
 *           capture beside it as a small ROUND thumbnail.
 *
 * The shutter being centred at the bottom, with the round thumbnail to its
 * left, is the arrangement this window is recognised by.  Two earlier
 * versions of this file put the capture buttons down the right-hand edge
 * and floated a gear over the frame; both are gone.  Along with them went a
 * row of six round toggles - timer, brightness, HDR, a framing grid, a
 * camera switch - none of which this shell can honour, and every one drawn
 * at a size where Lucide's sun becomes four loose dots.
 *
 * PHIPIA HAS NO CAMERA DRIVER, so the frame is plain grey.  It is not a
 * placeholder photograph and it is not an error page: an empty viewfinder is
 * what an empty viewfinder looks like, and the window is honest about that
 * until something is actually feeding it.
 *
 * The one thing here that is not the reference's is the shutter's motion:
 * the frame flashes and the ring closes onto the button when it fires.
 */
#ifndef PHIPIA_PHIPIA_CAMERA_H
#define PHIPIA_PHIPIA_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/ui.h>

enum phipia_camera_status {
    PHIPIA_CAMERA_STATUS_OK = 0,
    PHIPIA_CAMERA_STATUS_NULL_ARGUMENT,
    PHIPIA_CAMERA_STATUS_NOT_INITIALIZED,
    PHIPIA_CAMERA_STATUS_BAD_INDEX,
    PHIPIA_CAMERA_STATUS_UNSUPPORTED_GEOMETRY,
    PHIPIA_CAMERA_STATUS_SURFACE_FAILURE
};

/* The two segments of the control in the middle of the top bar. */
enum phipia_camera_mode {
    PHIPIA_CAMERA_MODE_PHOTO = 0,
    PHIPIA_CAMERA_MODE_VIDEO,
    PHIPIA_CAMERA_MODE_COUNT
};

const char *phipia_camera_status_string(enum phipia_camera_status status);

enum phipia_camera_status phipia_camera_initialize(struct surface *canvas,
    struct ui_rect frame);
enum phipia_camera_status phipia_camera_set_frame(struct ui_rect frame);
struct ui_rect phipia_camera_bounds(void);
/* The frame alone, which is what a capture pipeline draws into. */
struct ui_rect phipia_camera_viewfinder_bounds(void);
/* The shutter, so a caller can put a pointer on it without guessing. */
struct ui_rect phipia_camera_capture_bounds(void);

enum phipia_camera_status phipia_camera_set_mode(enum phipia_camera_mode mode);
enum phipia_camera_status phipia_camera_set_focus(bool focused);

/*
 * Whether a capture pipeline is drawing into phipia_camera_viewfinder_bounds().
 *
 * With a feed the window leaves those pixels alone.  Without one the frame
 * is plain grey - see the note above.  False until something says
 * otherwise.
 */
enum phipia_camera_status phipia_camera_set_feed(bool present);

/*
 * Fire the shutter.  The flash and the closing ring are animated, so the
 * caller has to keep calling phipia_camera_animate() until it says the motion is
 * finished.  With no feed there is nothing to put in the thumbnail, so the
 * thumbnail stays empty and the animation still runs.
 */
enum phipia_camera_status phipia_camera_capture(struct ui_rect *damage);

enum phipia_camera_status phipia_camera_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum phipia_camera_status phipia_camera_pointer_press(struct ui_point point,
    struct ui_rect *damage);

bool phipia_camera_animate(struct ui_rect *damage);
bool phipia_camera_animating(void);

enum phipia_camera_status phipia_camera_draw(struct ui_rect damage);

bool phipia_camera_self_test(void);
const char *phipia_camera_self_test_failure(void);

#endif
