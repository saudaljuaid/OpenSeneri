/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * File Explorer.
 *
 * Windows 10's is six horizontal bands stacked in a fixed order, and most of
 * that order is what makes a copy recognisable:
 *
 *   the TITLE BAR;
 *   the COMMAND BAR;
 *   the ADDRESS BAR - back, forward, up, the breadcrumb and a search box
 *   pinned to the right;
 *   the BODY - navigation tree on the left, the files on the right under a
 *   sortable column header;
 *   and the STATUS BAR, the item count at one end and the two view toggles
 *   at the other.
 *
 * The one band that is NOT Windows 10's is the second, and it is the reason
 * this window exists in the shape it does.
 *
 * WHAT THE RIBBON WAS.  Windows 10 puts a four-tab strip over a five-group
 * ribbon there: about twenty buttons and a hundred and twenty-four pixels
 * of chrome, of which a faithful copy could make perhaps four actually do
 * anything.  The rest were pictures of buttons.  So were the Quick Access
 * Toolbar in the title bar, the refresh mark in the address field, the
 * three arrows beside it, the column headings, the breadcrumb segments and
 * the two view toggles in the status bar - every one of them drawn exactly
 * as the control it was imitating, and every one inert.  A window that
 * looks like it can do thirty things and can do four is not a copy of File
 * Explorer; it is a screenshot you can hover.
 *
 * WHAT REPLACED IT.  One row of eight controls, forty pixels tall: New,
 * then Cut, Copy and Paste, then Rename and Delete, then Sort and View at
 * the right-hand end.  All eight work.  So does everything else listed
 * above - the arrows walk the navigation pane's own history and grey out
 * when there is nowhere to go, a breadcrumb segment that names a place in
 * the tree is a link and one that does not is plain text, the headings
 * sort, and the toggles switch views.  Nothing in this window is drawn as
 * a control unless pressing it does the thing it is drawn as.
 *
 * What Phipia does differently beyond that, and on purpose:
 *
 *   File type icons carry COLOUR.  Windows 10 draws every one of them in the
 *   same grey, so a folder of two hundred files is two hundred identical
 *   marks; here a picture is teal, a sound is violet, an archive is amber,
 *   and the eye finds what it is looking for without reading a word.
 *
 *   Folders and drives are ARTWORK rather than line marks - the yellow
 *   folder in the list, the orange one for a place in the tree, the drive
 *   for a volume.  Two hues of one shape is what tells a folder that is
 *   here from a folder that is somewhere else, and it costs one colour
 *   rather than five separate drawings the way Windows' known folders do.
 *
 *   A selected row gets an ACCENT BAR down its left edge and a soft fill,
 *   rather than Windows' flat blue plate.  It is the same idea the Start
 *   menu's list uses, so the two agree.
 *
 *   The breadcrumb's LAST segment is the accent colour.  Windows draws all of
 *   them the same and leaves you to work out where you are.
 *
 *   TWO view modes, not eight.  A table when you want the columns and big
 *   icons when you are looking at pictures; the six in between are
 *   decisions nobody makes twice.
 */
#ifndef PHIPIA_EXPLORER_H
#define PHIPIA_EXPLORER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cursor.h>
#include <phipia/surface.h>
#include <phipia/ui.h>

#define EXPLORER_MAX_ITEMS 24U
#define EXPLORER_MAX_PLACES 20U
#define EXPLORER_MAX_CRUMBS 6U
#define EXPLORER_NAME_BYTES 48U
#define EXPLORER_FIELD_BYTES 24U

enum explorer_status {
    EXPLORER_STATUS_OK = 0,
    EXPLORER_STATUS_NULL_ARGUMENT,
    EXPLORER_STATUS_NOT_INITIALIZED,
    EXPLORER_STATUS_BAD_INDEX,
    EXPLORER_STATUS_UNSUPPORTED_GEOMETRY,
    EXPLORER_STATUS_SURFACE_FAILURE
};

/*
 * What a row is, which decides both its icon and its colour.  Windows 10
 * decides the icon from the extension and draws them all grey; this decides
 * the colour from the same thing.
 */
enum explorer_kind {
    EXPLORER_FOLDER = 0,
    EXPLORER_TEXT,
    EXPLORER_IMAGE,
    EXPLORER_AUDIO,
    EXPLORER_VIDEO,
    EXPLORER_ARCHIVE,
    EXPLORER_CODE,
    EXPLORER_GENERIC,
    EXPLORER_KIND_COUNT
};

struct explorer_item {
    bool present;
    bool selected;
    enum explorer_kind kind;
    char name[EXPLORER_NAME_BYTES];
    char modified[EXPLORER_FIELD_BYTES];
    char type[EXPLORER_FIELD_BYTES];
    char size[EXPLORER_FIELD_BYTES];
};

/* One line of the navigation tree.  Depth 0 is a root, 1 is a child of the
 * root above it; Windows 10's tree never goes deeper than that at rest. */
struct explorer_place {
    bool present;
    bool expandable;
    bool expanded;
    bool current;
    bool pinned;            /* draws the pin Quick access puts on its own */
    uint8_t depth;
    char label[EXPLORER_NAME_BYTES];
    /*
     * What to draw beside the label.  Any name from assets/icons/explorer
     * (line marks), assets/icons/shell (illustrations), or the artwork in
     * assets/icons/app that this window carries - "folder-orange" for a
     * known folder, "drive" for a volume.  NULL draws the orange folder,
     * which is what a place in this tree almost always is.
     *
     * Pick one that says what the row IS.  An earlier tree gave Documents
     * a text-file page, Pictures a photo and Music a note: three folders
     * drawn as three files, which is the kind of icon that costs a second
     * every time it is read.
     */
    const char *glyph;
};

enum explorer_action_kind {
    EXPLORER_ACTION_NONE = 0,
    EXPLORER_ACTION_CREATE,
    EXPLORER_ACTION_RENAME,
    EXPLORER_ACTION_DELETE,
    EXPLORER_ACTION_COPY,
    EXPLORER_ACTION_MOVE
};

struct explorer_action {
    enum explorer_action_kind kind;
    enum explorer_kind item_kind;
    char source[EXPLORER_NAME_BYTES];
    char destination[EXPLORER_NAME_BYTES];
};

const char *explorer_status_string(enum explorer_status status);

enum explorer_status explorer_initialize(struct surface *canvas,
    struct ui_rect frame);
enum explorer_status explorer_set_frame(struct ui_rect frame);
struct ui_rect explorer_bounds(void);

enum explorer_status explorer_set_item(size_t index,
    const struct explorer_item *item);
enum explorer_status explorer_set_place(size_t index,
    const struct explorer_place *place);
/* The breadcrumb, one segment per call; the last one drawn is "where you
 * are" and takes the accent colour. */
enum explorer_status explorer_set_crumb(size_t index, const char *label);
enum explorer_status explorer_set_title(const char *title);
enum explorer_status explorer_set_focus(bool focused);
/*
 * Which view the list is in, and how it is ordered - the two settings the
 * View and Sort menus hold, so a caller can open the window in either
 * without driving the menus to get there.
 *
 * Sorting is over the strings the caller supplied: a date is read as
 * dd/mm/yyyy hh:mm and anything else sorts newest, a size is read as a
 * number and a unit, and folders come before files in every column the way
 * Windows does it.  Ties fall back to the name.
 */
enum explorer_sort {
    EXPLORER_SORT_NAME = 0,
    EXPLORER_SORT_MODIFIED,
    EXPLORER_SORT_TYPE,
    EXPLORER_SORT_SIZE,
    EXPLORER_SORT_COUNT
};

enum explorer_status explorer_set_view(bool tiles);
enum explorer_status explorer_set_sort(enum explorer_sort column,
    bool descending);

/*
 * Create a new item of the given kind at the first free row, select it, and
 * open the inline rename Windows opens for a freshly created one - the name
 * highlighted and ready to be typed over.  Collides with an existing name by
 * appending " (2)", " (3)" and so on, which is what Windows does rather than
 * refuse the name outright.
 */
enum explorer_status explorer_create_item(enum explorer_kind kind,
    struct ui_rect *damage);
/* Whether a row is mid-rename and therefore one of the two things
 * explorer_text_input() and the three key functions below can be acting on -
 * see explorer_command_palette_open() for the other. */
bool explorer_renaming(void);

/*
 * Ctrl+K.  Opens or closes a Phipia addition with nothing in Windows 10 to
 * copy: a filtered list of the commands this window can actually carry out
 * - New folder, New Text Document, New Bitmap image, Select all/none,
 * switching the view, and "Go to <place>" for every row in the nav tree -
 * typed to rather than hunted for across a bar and a tree.  Refuses to
 * open while a row is mid-rename, and vice versa; the two are not something
 * this window lets happen at once.
 */
enum explorer_status explorer_toggle_command_palette(struct ui_rect *damage);
bool explorer_command_palette_open(void);

/*
 * The search box in the address bar, which FILTERS the list rather than
 * decorating the bar - it was drawn and did nothing at all before this.
 *
 * Windows 10 searches the folder: it walks what is on disk under where you
 * are standing, finds matches in sub-folders too, and runs a progress bar
 * along the address bar while it works.  This narrows the rows the window
 * was handed, which is that idea's honest form for a shell whose list
 * arrives through explorer_set_item() instead of off a volume - everything
 * a query could match is already on screen, so there is nothing to walk
 * and nobody to keep waiting.  The match is case-insensitive and anywhere
 * in the name, so "port" finds "reports", exactly as Windows' does.
 *
 * A filtered row is removed from the layout rather than skipped while
 * drawing, so the rows, the hit targets, the selection and the count in
 * the status bar cannot disagree about what is in the folder.  That count
 * reads "2 of 3 items" while a filter is on, because a window that hides
 * rows and does not say so looks like a folder that lost files.
 *
 * Focus is exclusive with the command palette and with a rename: this
 * shell has one blinking caret, and taking it for one box gives it up
 * from the other.  Focusing is refused outright while a row is mid-rename,
 * the same way opening the palette is.
 */
enum explorer_status explorer_focus_search(bool wanted,
    struct ui_rect *damage);
/* Empties the box and puts every filtered row back, without touching the
 * caret - what the x at the box's right end does, and what the palette's
 * "Clear search" runs.  That command is offered only while there is a
 * query to clear. */
enum explorer_status explorer_clear_search(struct ui_rect *damage);
bool explorer_search_focused(void);
/* The query itself, "" when the box is empty; never NULL. */
const char *explorer_search_query(void);
/* How many rows the filter is letting through - what the status bar
 * counts, and what "select all" acts on. */
size_t explorer_visible_item_count(void);

/*
 * Feed to a window that is renaming a row OR has the command palette open;
 * harmless to call when neither explorer_renaming() nor
 * explorer_command_palette_open() is true, since there is then nothing to
 * act on.  Reserved filename characters (\ / : * ? " < > |) are dropped from
 * a rename rather than inserted, which is what Windows' own edit box does
 * rather than accepting them and refusing the name later; the palette has
 * no such restriction; it is filtering text, not naming a file.
 */
enum explorer_status explorer_text_input(char character,
    struct ui_rect *damage);
enum explorer_status explorer_key_backspace(struct ui_rect *damage);
/*
 * Commits.  For a rename, an empty name is refused and editing continues,
 * since Windows will not create a nameless file either.  For the palette,
 * runs the FIRST command in the current filtered list and closes it - the
 * one a keyboard-only user would expect Enter to run, since there is no way
 * yet to arrow down to a different one; a mouse click on any row runs that
 * row directly and is not limited to the first.
 */
enum explorer_status explorer_key_enter(struct ui_rect *damage);
/*
 * Cancels.  For the palette, closes it without running anything.  For a
 * rename, what it undoes depends on how the rename started: a row that
 * exists only because New made it goes with the rename - what Windows does
 * to a New folder abandoned before it is named - and a row that was
 * already there gets its old name back and stays.
 */
enum explorer_status explorer_key_escape(struct ui_rect *damage);

/*
 * Which pointer belongs at a point, which this window is the only thing
 * that can answer: an edge or a corner is a resize, the three boxes that
 * take typing are a caret, a breadcrumb segment is a link, and everything
 * else is the arrow.
 *
 * Before this the shapes existed and nothing chose between them - the
 * walkthrough in tools/preview/tour.c named one per scene by hand, which
 * shows a pointer changing without anything actually deciding that it
 * should.  A caller drives the real thing by asking this on every pointer
 * move and handing the answer to cursor_set_kind().
 *
 * Busy and Working in background are deliberately never returned.  They do
 * not depend on where the pointer is - they say the machine is thinking,
 * which only the code doing the thinking knows, so they stay the caller's
 * to set around whatever is taking the time.
 */
enum cursor_kind explorer_cursor_at(struct ui_point point);

enum explorer_status explorer_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum explorer_status explorer_pointer_press(struct ui_point point,
    struct ui_rect *damage);
/* Consume the filesystem mutation requested by the last completed command.
 * The shell performs it against its real volume, then reloads the rows. */
bool explorer_take_action(struct explorer_action *action);

/* Advance the hover cross-fades and the rename caret's blink to the
 * monotonic clock.  Returns true while another frame is still owed. */
bool explorer_animate(struct ui_rect *damage);
bool explorer_animating(void);

enum explorer_status explorer_draw(struct ui_rect damage);

bool explorer_self_test(void);
const char *explorer_self_test_failure(void);

#endif
