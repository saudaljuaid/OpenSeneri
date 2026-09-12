/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * The Store.
 *
 * Windows 10's is a top navigation bar over a scrolling page of horizontal
 * shelves: a spotlight panel at the head, then rows of app cards under
 * headings with a "Show all" at the right of each. No left rail - the Store
 * is the one first-party Windows 10 window that navigates across the top.
 *
 * What Phipia does differently:
 *
 *   A card is BUILT FROM ITS APP'S OWN COLOUR. Windows draws every tile on
 *   the same white card and lets the logos fight the background; here the
 *   plate behind the logo is the app's colour, the card's foot picks up a
 *   tint of it, and a shelf of eight apps reads as eight things rather than
 *   as eight white rectangles.
 *
 *   The rating is a BAR, not five little stars. Five stars at card size is
 *   twenty pixels of shape carrying one number badly; a bar carries the same
 *   number at a glance and leaves room for the count beside it.
 *
 *   The spotlight is the same card, wide - so the head of the page is
 *   obviously the same kind of thing as the shelves under it, rather than a
 *   carousel with its own rules.
 *
 * There is no catalogue in here. The Store knows the shape of a card and
 * nothing about what is in one: every app is handed over by the caller, the
 * same way the taskbar is handed its buttons.
 */
#ifndef PHIPIA_STORE_H
#define PHIPIA_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/ui.h>

#define STORE_MAX_APPS 24U
#define STORE_MAX_SHELVES 3U
#define STORE_TEXT_BYTES 40U

enum store_status {
    STORE_STATUS_OK = 0,
    STORE_STATUS_NULL_ARGUMENT,
    STORE_STATUS_NOT_INITIALIZED,
    STORE_STATUS_BAD_INDEX,
    STORE_STATUS_UNSUPPORTED_GEOMETRY,
    STORE_STATUS_SURFACE_FAILURE
};

/*
 * One app on a shelf.
 *
 * `art` is a name from the built-in artwork - the same pictures the taskbar
 * draws, fetched through taskbar_artwork() rather than copied.  `colour` is
 * the plate behind it, packed the way the framebuffer packs a pixel; zero
 * takes a neutral.  `rating` is 0..50, so 45 is four and a half.
 */
struct store_app {
    bool present;
    bool spotlight;          /* drawn wide, at the head of the page */
    uint8_t shelf;
    uint8_t rating;          /* tenths of a star, 0..50 */
    char name[STORE_TEXT_BYTES];
    char category[STORE_TEXT_BYTES];
    char price[STORE_TEXT_BYTES];
    char tagline[STORE_TEXT_BYTES];   /* spotlight only */
    char reviews[STORE_TEXT_BYTES];
    const char *art;
    uint32_t colour;
};

const char *store_status_string(enum store_status status);

enum store_status store_initialize(struct surface *canvas,
    struct ui_rect frame);
enum store_status store_set_frame(struct ui_rect frame);
struct ui_rect store_bounds(void);
enum store_status store_primary_action_bounds(struct ui_rect *bounds);

enum store_status store_set_app(size_t index, const struct store_app *app);
enum store_status store_set_shelf(size_t shelf, const char *heading);
enum store_status store_set_tab(size_t tab);
enum store_status store_set_focus(bool focused);

enum store_status store_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum store_status store_pointer_press(struct ui_point point,
    struct ui_rect *damage);

/* Advance the hover cross-fade to the monotonic clock.  Returns true while
 * another frame is still owed. */
bool store_animate(struct ui_rect *damage);
bool store_animating(void);

enum store_status store_draw(struct ui_rect damage);

bool store_self_test(void);
const char *store_self_test_failure(void);

#endif
