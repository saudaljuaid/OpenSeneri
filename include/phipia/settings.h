/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Settings.
 *
 * Windows 10's Settings is two screens.  The HOME is four bands stacked in a
 * white window - the account, the title, the search box, and then the grid
 * of category tiles - and is the whole picture people have of this app.  A
 * tile opens a CATEGORY PAGE: a heading, and a column of settings rows.
 *
 * A tile is an icon on the left with the category's name beside it and a
 * one-line summary of what is inside under that.  Thirteen of them, three to
 * a row.  The summaries matter as much as the names: "Display, sound,
 * notifications, power" is how anybody actually finds System, and a copy that
 * leaves them out has the shape of the window without the thing it is for.
 *
 * What Phipia does differently:
 *
 *   A hovered tile takes a soft plate and an accent bar down its left edge -
 *   the treatment the Start menu's list and the file list already use.
 *
 * A tile CAN carry its own colour - see `colour` below - and by default does
 * not, which is a decision rather than an omission and the opposite of the
 * one the file list makes.  An earlier version of the preview gave all
 * twelve their own hue, picked as an even sweep of the wheel, on the
 * argument that twelve identical marks make you read every label to find
 * one.  The argument was fine and the grid still looked like a bag of
 * sweets: twelve hues standing for nothing are decoration, and they fought
 * the single accent the rest of the window is built on.  A file list's
 * colours mean something - teal is a picture, violet is a sound - and there
 * is no equivalent for "Gaming".  Set `colour` where a category has a
 * meaning worth a hue; leave it zero otherwise.
 *
 * The categories are supplied by the caller.  Settings knows the shape of a
 * tile and nothing about what a system has in it.
 */
#ifndef PHIPIA_SETTINGS_H
#define PHIPIA_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/cursor.h>
#include <phipia/ui.h>

#define SETTINGS_MAX_TILES 16U
#define SETTINGS_TEXT_BYTES 48U

enum settings_status {
    SETTINGS_STATUS_OK = 0,
    SETTINGS_STATUS_NULL_ARGUMENT,
    SETTINGS_STATUS_NOT_INITIALIZED,
    SETTINGS_STATUS_BAD_INDEX,
    SETTINGS_STATUS_UNSUPPORTED_GEOMETRY,
    SETTINGS_STATUS_SURFACE_FAILURE
};

/*
 * One category.  `glyph` is a Lucide name from assets/icons/settings and
 * `colour` is packed the way the framebuffer packs a pixel; zero takes the
 * system accent, which is what every one of these is on Windows.
 */
struct settings_tile {
    bool present;
    char name[SETTINGS_TEXT_BYTES];
    char summary[SETTINGS_TEXT_BYTES];
    const char *glyph;
    uint32_t colour;
};

/*
 * WHAT A ROW IS, and what "it works" means for one.
 *
 * A category page is a column of these.  Five kinds cover what Windows 10
 * puts on a Settings page, and every one of them does the thing it is drawn
 * as: a switch flips, a choice opens its list and takes the row you pick, a
 * slider takes the position you press, a button reports itself, a heading is
 * a heading and is not hoverable because it is not a control.
 *
 * WHAT THEY DO NOT DO is reach a machine.  Flipping "Bluetooth" here moves
 * this window's switch; it does not touch a radio, because there is no radio
 * behind this window to touch and pretending otherwise would make every row
 * a lie rather than a limitation.  The state is real, it is this window's,
 * and settings_row_state() hands it back - a caller that HAS a radio is the
 * thing that would join the two.  That is the same line the file list's
 * clipboard draws, and for the same reason.
 */
enum settings_row_kind {
    SETTINGS_ROW_HEADING = 0,   /* a group title; no control, no hover */
    SETTINGS_ROW_TOGGLE,        /* a switch, On or Off */
    SETTINGS_ROW_CHOICE,        /* one of `options`, chosen from a flyout */
    SETTINGS_ROW_SLIDER,        /* 0 to 100, set to where you press */
    SETTINGS_ROW_ACTION,        /* a button; the press is reported back */
    SETTINGS_ROW_KIND_COUNT
};

#define SETTINGS_MAX_ROWS 9U
#define SETTINGS_MAX_OPTIONS 4U
#define SETTINGS_OPTION_BYTES 24U

struct settings_row {
    bool present;
    enum settings_row_kind kind;
    char label[SETTINGS_TEXT_BYTES];
    /* The line under the label.  Optional, and worth having for the same
     * reason a tile's summary is: it is where the answer to "which one is
     * this?" actually lives. */
    char detail[SETTINGS_TEXT_BYTES];
    /* TOGGLE: on or off.  SLIDER: 0 to 100.  CHOICE: which option is
     * picked.  ACTION: unused. */
    uint32_t state;
    /*
     * CHOICE: the list, ending at the first empty one.  ACTION: options[0]
     * is the word on the BUTTON - "Check", "Get started" - which is not the
     * row's label and should not repeat it, because the label says what the
     * setting is and the button says what pressing it does.  Every other
     * kind leaves this empty.
     */
    char options[SETTINGS_MAX_OPTIONS][SETTINGS_OPTION_BYTES];
};

const char *settings_status_string(enum settings_status status);

enum settings_status settings_initialize(struct surface *canvas,
    struct ui_rect frame);
enum settings_status settings_set_frame(struct ui_rect frame);
struct ui_rect settings_bounds(void);

/* A caret in the search box, an arrow over every control.  See
 * phipia/cursor.h. */
enum cursor_kind settings_cursor_at(struct ui_point point);

enum settings_status settings_set_tile(size_t index,
    const struct settings_tile *tile);
/*
 * A row on a category page.  `page` is the tile's own index, so a category
 * and its rows are addressed the same way and cannot drift apart.
 */
enum settings_status settings_set_row(size_t page, size_t index,
    const struct settings_row *row);
/* What a row is currently set to - the switch, the slider, the choice.  A
 * caller with real hardware behind it reads this to find out what was asked
 * for; the window itself does nothing else with it. */
uint32_t settings_row_state(size_t page, size_t index);

/*
 * NAVIGATION.  Home is (size_t)-1; anything else is the tile whose page is
 * open.  Clicking a tile opens it, the caption's back arrow returns - and
 * that arrow is drawn faint on the home page because there is genuinely
 * nowhere behind it, which is the one place it is allowed to be a picture.
 */
size_t settings_open_page(void);
enum settings_status settings_go_to_page(size_t page, struct ui_rect *damage);
enum settings_status settings_go_back(struct ui_rect *damage);
/*
 * The last ACTION row pressed, cleared by reading it - a button on a page
 * this window cannot act on has to hand the press to somebody, and this is
 * where it puts it.  False when nothing is waiting.
 */
bool settings_take_action(size_t *page, size_t *index);

/*
 * THE SEARCH BOX, which filters and then navigates rather than decorating
 * the home page - it was drawn with "Find a setting" in it and took no
 * keystroke at all before this.
 *
 * Windows 10 searches the whole system and reaches into Control Panel for
 * what Settings has not absorbed yet.  This searches what the window was
 * handed: every category's name and summary, and every row on every page.
 * That is the honest form of the same idea here - everything it could match
 * is already loaded, so there is nothing to go and look up and nobody to
 * keep waiting.  The match is case-insensitive and anywhere in the string,
 * so "blue" finds Bluetooth.
 *
 * A result names the category it lives in, because "Night light" on its own
 * does not tell you where you are about to be taken.  Running one opens
 * that page with the row lit, which is what Windows does.
 *
 * The box is on the HOME page only, where Windows makes it the centre of
 * the screen; a category page has a heading there instead.
 */
enum settings_status settings_focus_search(bool wanted,
    struct ui_rect *damage);
bool settings_search_focused(void);
const char *settings_search_query(void);
size_t settings_search_result_count(void);
enum settings_status settings_text_input(char character,
    struct ui_rect *damage);
enum settings_status settings_key_backspace(struct ui_rect *damage);
/* Runs the first result and opens its page, the way the palette in File
 * Explorer runs its top row - there is no way to arrow down to another yet,
 * and a click takes whichever result it lands on. */
enum settings_status settings_key_enter(struct ui_rect *damage);
/* Empties the box and drops the caret; on a category page, goes back. */
enum settings_status settings_key_escape(struct ui_rect *damage);
enum settings_status settings_set_account(const char *name,
    const char *detail);
enum settings_status settings_set_heading(const char *heading);
enum settings_status settings_set_focus(bool focused);

enum settings_status settings_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum settings_status settings_pointer_press(struct ui_point point,
    struct ui_rect *damage);

/* Advance the hover cross-fade to the monotonic clock.  Returns true while
 * another frame is still owed. */
bool settings_animate(struct ui_rect *damage);
bool settings_animating(void);

enum settings_status settings_draw(struct ui_rect damage);

bool settings_self_test(void);
const char *settings_self_test_failure(void);

#endif
