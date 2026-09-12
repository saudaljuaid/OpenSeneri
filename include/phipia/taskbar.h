/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_TASKBAR_H
#define PHIPIA_TASKBAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/cursor.h>
#include <phipia/ui.h>

/*
 * A Windows 11 taskbar, drawn by hand.
 *
 * Every measurement in the implementation is a named constant carrying the
 * source it was taken from, and the two files that own them - taskbar.c for
 * the bar and taskbar_flyout.c for what it opens - contain no number that is
 * not either measured, cited, or derived in view of the reader.
 *
 * The taskbar owns no windows.  It is told what exists through
 * taskbar_set_app() and it reports what the user did through
 * taskbar_take_action(); the compositor above it decides what that means.
 * This keeps the bar portable across Phipia's panels and its native windows
 * without either of them having to know how a taskbar is drawn.
 */

#define TASKBAR_MAX_APPS 12U
#define TASKBAR_MAX_TRAY 6U
#define TASKBAR_LABEL_BYTES 32U
#define TASKBAR_JUMP_ITEM_BYTES 40U
#define TASKBAR_MAX_JUMP_ITEMS 6U
#define TASKBAR_SEARCH_BYTES 40U

enum taskbar_status {
    TASKBAR_STATUS_OK = 0,
    TASKBAR_STATUS_NULL_ARGUMENT,
    TASKBAR_STATUS_NOT_INITIALIZED,
    TASKBAR_STATUS_ALREADY_INITIALIZED,
    TASKBAR_STATUS_UNSUPPORTED_GEOMETRY,
    TASKBAR_STATUS_BAD_INDEX,
    TASKBAR_STATUS_BAD_ELEMENT,
    TASKBAR_STATUS_RECTANGLE_OVERFLOW,
    TASKBAR_STATUS_SURFACE_FAILURE,
    TASKBAR_STATUS_FONT_FAILURE,
    TASKBAR_STATUS_CLOCK_FAILURE
};

/* Windows 11 ships one taskbar height and one reduced height; both are here
 * because the reduced one is what a 600-pixel-tall screen can afford. */
enum taskbar_size {
    TASKBAR_SIZE_DEFAULT = 0,
    TASKBAR_SIZE_SMALL
};

enum taskbar_alignment {
    TASKBAR_ALIGNMENT_CENTER = 0,
    TASKBAR_ALIGNMENT_LEFT
};

enum taskbar_theme {
    TASKBAR_THEME_DARK = 0,
    TASKBAR_THEME_LIGHT
};

/*
 * The four shapes Windows offers the search entry point, which are exactly
 * the four choices in Settings > Personalization > Taskbar > Search.  A box
 * that will not fit collapses to an icon rather than crowding the tray, which
 * is what Windows itself does on a narrow screen.
 */
enum taskbar_search_mode {
    TASKBAR_SEARCH_HIDDEN = 0,
    TASKBAR_SEARCH_ICON,
    TASKBAR_SEARCH_ICON_LABEL,
    TASKBAR_SEARCH_BOX
};

/*
 * The running state of one taskbar button.  Windows 11 draws a different
 * indicator for each of these and nothing else distinguishes them.
 */
enum taskbar_run_state {
    TASKBAR_RUN_PINNED = 0,   /* pinned, no window: no indicator at all */
    TASKBAR_RUN_BACKGROUND,   /* one window, not foreground */
    TASKBAR_RUN_FOREGROUND,   /* one window, foreground */
    TASKBAR_RUN_GROUPED,      /* several windows, not foreground */
    TASKBAR_RUN_GROUPED_FOCUS /* several windows, foreground */
};

/*
 * The built-in marks.  Every one of them is a Lucide icon, rasterized offline
 * by tools/make-lucide-glyphs.py into src/kernel/taskbar_glyphs.h - Segoe MDL2
 * Assets is not redistributable, and Lucide's grid, stroke weight and round
 * terminals are close enough to it that the bar reads correctly.
 *
 * TASKBAR_GLYPH_START is the exception: the Start mark is Phipia's own logo,
 * carried as geometry in taskbar.c rather than as a Lucide icon.  A caller
 * that wants a different one hands over a bitmap through
 * taskbar_set_start_icon().
 */
enum taskbar_glyph {
    TASKBAR_GLYPH_NONE = 0,
    TASKBAR_GLYPH_START,
    TASKBAR_GLYPH_SEARCH,
    TASKBAR_GLYPH_TASK_VIEW,
    TASKBAR_GLYPH_FILE_EXPLORER,
    TASKBAR_GLYPH_TERMINAL,
    TASKBAR_GLYPH_NOTES,
    TASKBAR_GLYPH_CAMERA,
    TASKBAR_GLYPH_CANVAS,
    TASKBAR_GLYPH_STORE,
    TASKBAR_GLYPH_SETTINGS,
    TASKBAR_GLYPH_CHEVRON_UP,
    TASKBAR_GLYPH_NETWORK,
    TASKBAR_GLYPH_VOLUME,
    TASKBAR_GLYPH_BATTERY,
    TASKBAR_GLYPH_ACTION_CENTER,
    TASKBAR_GLYPH_POWER,
    TASKBAR_GLYPH_ACCOUNT,
    TASKBAR_GLYPH_MENU,
    TASKBAR_GLYPH_PICTURES,
    TASKBAR_GLYPH_COUNT
};

/*
 * An app icon supplied by the host.  Phipia already decodes its application
 * icons into a colour plane and a separate alpha plane, so the taskbar takes
 * exactly that pair rather than asking the caller to repack anything.  A NULL
 * pixel plane falls back to the built-in glyph.
 */
struct taskbar_icon {
    /*
     * Built-in artwork by name - "start", "files", "terminal", "camera",
     * "store" - which the taskbar carries at every size it draws at and picks
     * between at draw time.  This is the sharpest option, because each size
     * was resampled from the original rather than from a larger cell.
     */
    const char *art;
    /* Or artwork the caller decoded itself, at one size. */
    const uint32_t *pixels;
    const uint8_t *alpha;
    uint32_t width;
    uint32_t height;
    /* Or, failing both, one of the built-in Lucide marks in a flat colour. */
    enum taskbar_glyph glyph;
    uint32_t glyph_colour;
};

struct taskbar_app {
    bool present;
    char label[TASKBAR_LABEL_BYTES];
    struct taskbar_icon icon;
    enum taskbar_run_state run;
    uint8_t window_count;
    uint8_t badge;      /* 0 draws no badge; 1..99 draws the count */
    bool attention;     /* the flashing state Windows calls "needs attention" */
    /* 0 draws no progress bar; 1..100 draws one under the icon, which is
     * where Windows puts a download's progress. */
    uint8_t progress;
    enum ui_panel_id panel;
};

/*
 * What the Start button opens.
 *
 * Windows 10's Start menu shows an alphabetical application list beside a
 * grid of tiles, and both are the compositor's to fill: the taskbar knows the
 * shape of the menu, not what belongs in it.
 *
 * A list entry with `heading` set is a divider - a single letter, or "Most
 * used" - drawn in the accent colour rather than as a row.  Entries are drawn
 * in index order, so the caller sorts.
 */
#define TASKBAR_MAX_START_ENTRIES 24U
#define TASKBAR_MAX_START_TILES 16U
#define TASKBAR_MAX_START_GROUPS 2U

struct taskbar_start_entry {
    bool present;
    bool heading;
    char label[TASKBAR_LABEL_BYTES];
    struct taskbar_icon icon;
    enum ui_panel_id panel;
};

/*
 * A tile, on the grid of small squares Windows measures them in.  Its Start
 * layout XML names the four sizes by that grid - 1x1, 2x2, 4x2 and 4x4 - and
 * a group is six columns across, which is why exactly three medium tiles fit
 * a row.  `colour` is packed the way the framebuffer packs a pixel; zero
 * takes the system accent, which is what an application that ships no tile
 * colour gets on Windows.
 */
struct taskbar_start_tile {
    bool present;
    char label[TASKBAR_LABEL_BYTES];
    struct taskbar_icon icon;
    uint8_t group;
    uint8_t column;
    uint8_t row;
    uint8_t columns;
    uint8_t rows;
    uint32_t colour;
    enum ui_panel_id panel;
};

/* What the user asked the compositor to do.  The taskbar never acts. */
enum taskbar_action_kind {
    TASKBAR_ACTION_NONE = 0,
    TASKBAR_ACTION_LAUNCH,        /* activate a pinned app */
    TASKBAR_ACTION_ACTIVATE,      /* raise an existing window */
    TASKBAR_ACTION_MINIMIZE,      /* the foreground app was clicked again */
    TASKBAR_ACTION_NEW_INSTANCE,  /* middle click, or shift+click */
    TASKBAR_ACTION_CLOSE,         /* "Close window" from the jump list */
    TASKBAR_ACTION_PIN,
    TASKBAR_ACTION_UNPIN,
    TASKBAR_ACTION_SHOW_DESKTOP,
    TASKBAR_ACTION_TASK_VIEW,
    TASKBAR_ACTION_START,          /* the Start button */
    TASKBAR_ACTION_SEARCH,
    TASKBAR_ACTION_WIDGETS,
    TASKBAR_ACTION_TRAY_OVERFLOW,  /* the chevron */
    TASKBAR_ACTION_NETWORK,
    TASKBAR_ACTION_VOLUME,
    TASKBAR_ACTION_BATTERY,
    TASKBAR_ACTION_NOTIFICATIONS,  /* the Action Center button */
    TASKBAR_ACTION_CALENDAR,       /* the clock */
    TASKBAR_ACTION_OPEN_SETTINGS,
    /* From inside the Start menu. */
    TASKBAR_ACTION_START_ENTRY,    /* a row of the application list */
    TASKBAR_ACTION_START_TILE,
    TASKBAR_ACTION_DOCUMENTS,
    TASKBAR_ACTION_PICTURES,
    TASKBAR_ACTION_ACCOUNT,
    TASKBAR_ACTION_POWER,
    /*
     * Task Manager, which is reached by Alt+F4 or by searching for it and
     * is deliberately NOT a button on the bar - Windows does not pin it
     * either, and a diagnostic you only want when something is wrong is
     * the wrong thing to spend a permanent slot on.
     */
    TASKBAR_ACTION_TASK_MANAGER
};

struct taskbar_action {
    enum taskbar_action_kind kind;
    size_t app_index;   /* also the entry or tile index, for the two above */
    enum ui_panel_id panel;
};

/*
 * What the installed taskbar measured, so that a boot can say what it drew
 * rather than that it drew something.  Every field is compared against the
 * documented Windows 11 value by taskbar_verify_installed(); the proof exists
 * so that a change which quietly moves the bar by a pixel is a failure rather
 * than a thing somebody notices later in a screenshot.
 */
struct taskbar_proof {
    uint32_t bar_height;
    uint32_t button_extent;
    uint32_t panel_size;
    uint32_t icon_size;
    uint32_t corner_radius;
    uint32_t indicator_height;
    uint32_t indicator_bottom_gap;
    uint32_t tray_slot_width;
    uint32_t tray_glyph_size;
    uint32_t show_desktop_width;
    uint32_t cluster_slots;
    uint32_t cluster_width;
    uint32_t work_area_height;
    uint64_t draws;
};

struct taskbar_counters {
    uint64_t draws;
    uint64_t glyphs;
    uint64_t hover_changes;
    uint64_t activations;
    uint64_t clock_reads;
    uint64_t material_rebuilds;
};

/*
 * Build the bar for a screen.  A screen too short for the default height is
 * given the reduced one rather than a refusal, because a taskbar that is not
 * drawn is worse than a taskbar that is drawn small.
 */
enum taskbar_status taskbar_initialize(
    struct surface *surface,
    uint32_t width,
    uint32_t height
);
bool taskbar_is_initialized(void);
void taskbar_shutdown(void);

/*
 * The Start mark, which is artwork rather than an icon: a colour plane and a
 * separate alpha plane, the same pair Phipia already decodes its application
 * icons into.  Passing NULL clears it and the bar falls back to a geometric
 * stand-in, so a caller with no artwork still gets a Start button.
 */
enum taskbar_status taskbar_set_start_icon(const struct taskbar_icon *icon);

/* A caret in the search box, an arrow over every button.  The bar answers
 * last, because it is under every window; see phipia/cursor.h. */
enum cursor_kind taskbar_cursor_at(struct ui_point point);

enum taskbar_status taskbar_set_theme(enum taskbar_theme theme);
enum taskbar_status taskbar_set_alignment(enum taskbar_alignment alignment);
enum taskbar_status taskbar_set_size(enum taskbar_size size);
enum taskbar_status taskbar_set_accent(uint8_t red, uint8_t green,
    uint8_t blue);
enum taskbar_status taskbar_set_transparency(bool transparent);
/*
 * Whether to blur what is behind the bar.  Windows 10 does not: its Start
 * menu and Action Center are acrylic and its taskbar is a flat tint, which is
 * why wallpaper colour shows through it but wallpaper detail does not.  This
 * exists because turning it on is what third-party tools do to Windows 10,
 * and because it is what Windows 11 does.
 */
enum taskbar_status taskbar_set_blur(bool blur);
/* Windows 10 has an Action Center button right of the clock; it is off by
 * default here because there is nothing behind it to open. */
enum taskbar_status taskbar_set_action_center_visible(bool visible);
/* The tray's overflow chevron.  Off by default: Phipia has no hidden-icon
 * tray, and a chevron that opens nothing lies about there being more. */
enum taskbar_status taskbar_set_chevron_visible(bool visible);
enum taskbar_status taskbar_set_show_desktop_button(bool visible);
enum taskbar_status taskbar_set_widgets_visible(bool visible);
enum taskbar_status taskbar_set_search_visible(bool visible);
enum taskbar_status taskbar_set_search_mode(enum taskbar_search_mode mode);
/* Windows 10 pins Task View beside the search box; it is off by default
 * here because Phipia has no timeline and no virtual desktops. */
enum taskbar_status taskbar_set_task_view_visible(bool visible);

enum taskbar_status taskbar_set_app(size_t index, const struct taskbar_app *app);
enum taskbar_status taskbar_clear_app(size_t index);
enum taskbar_status taskbar_set_run_state(size_t index,
    enum taskbar_run_state run);
enum taskbar_status taskbar_set_badge(size_t index, uint8_t badge);
enum taskbar_status taskbar_set_progress(size_t index, uint8_t percent);
enum taskbar_status taskbar_set_attention(size_t index, bool attention);
/*
 * Keyboard focus.  Windows draws its focus ring two pixels outside the
 * button's panel and only while the taskbar is being driven from the
 * keyboard, so it is set explicitly rather than inferred from hover.
 */
enum taskbar_status taskbar_set_focus(size_t index);
enum taskbar_status taskbar_clear_focus(void);
size_t taskbar_app_count(void);

/* The rectangle the bar occupies, so the compositor can keep windows out. */
struct ui_rect taskbar_bounds(void);
/* The desktop work area left over once the bar has taken its strip. */
struct ui_rect taskbar_work_area(void);
/*
 * Where one application's button sits, in screen coordinates.  Windows
 * anchors a window's thumbnail preview to its button; a compositor needs the
 * rectangle to do the same, and recomputing the layout it was just handed
 * would be the wrong way to get it.  Refuses an index with no application on
 * it.
 */
enum taskbar_status taskbar_app_bounds(size_t index, struct ui_rect *bounds);
/*
 * How full the tray battery reads, 0 to 100.  Windows 10 draws the charge as
 * a solid bar inside the shell rather than as a fixed row of cells, so this
 * moves the icon rather than choosing between pictures of one.
 */
enum taskbar_status taskbar_set_battery(uint8_t percent);

/*
 * The built-in artwork, by name, at the size nearest the one asked for.
 *
 * The taskbar carries these because it draws them; every other part of the
 * shell that wants the same picture - a Store card, a Start tile - should
 * draw the SAME bytes rather than carry a second copy of them, which is what
 * including the generated table twice would do.  Returns false for a name
 * that is not built in.
 */
/*
 * The dominant colour of a named piece of shell artwork.
 *
 * This is the Color Hot-Track finder, which the taskbar uses to light a
 * hovered button in its application's own colour: a histogram over the
 * icon's hues, weighted by saturation so a mostly-white mark with one
 * coloured corner does not read as white, then lifted to full value so a
 * dark icon still yields its hue rather than black.
 *
 * It is exported because it is not a taskbar question.  Anything that wants
 * to put a surface behind an application's artwork needs the same answer,
 * and the Store does; two copies of a histogram are two places for it to
 * drift.  Returns false for a monochrome mark, which HAS no dominant
 * colour - the caller decides what neutral means.
 */
bool taskbar_artwork_tint(const char *name, uint32_t wanted, uint8_t *red,
    uint8_t *green, uint8_t *blue);

bool taskbar_artwork(const char *name, uint32_t wanted,
    const uint32_t **pixels, const uint8_t **alpha, uint32_t *side);

/*
 * The Start menu's contents, and whether it is showing.  Opening and closing
 * are animated, so both report the rectangle that has to be redrawn and the
 * caller must keep calling taskbar_animate() until it says the motion is
 * finished.
 */
enum taskbar_status taskbar_set_start_entry(size_t index,
    const struct taskbar_start_entry *entry);
enum taskbar_status taskbar_set_start_tile(size_t index,
    const struct taskbar_start_tile *tile);
enum taskbar_status taskbar_set_start_group(size_t group, const char *name);
enum taskbar_status taskbar_set_start_open(bool open, struct ui_rect *damage);
bool taskbar_start_menu_open(void);
struct ui_rect taskbar_start_menu_bounds(void);

/*
 * What "Type here to search" opens, which drew a box and did nothing at all
 * before this.
 *
 * Windows 10's search flyout answers a query in a fixed shape - a BEST
 * MATCH given a large icon and a line naming what kind of thing it is, then
 * the rest of what matched in named groups under it, then the box you are
 * typing into along the bottom edge - and this is that shape.  It rises the
 * same 48 pixels over the same quarter second the Start menu does, off the
 * same acrylic, from the same anchor: they are one gesture and would look
 * like two if they differed.  Only one of the two is ever open, which is
 * what lets them share a single blurred copy of the desktop.
 *
 * Everything it can find is something the shell was already told about.
 * The apps are the alphabetical list the Start menu draws, so
 * taskbar_set_start_entry() populates both at once; the utilities are the
 * destinations that menu's rail already offers, resolving to the same
 * taskbar_action kinds.  Nothing here is a result invented to fill a
 * picture, and there is nothing to configure separately from the Start
 * menu.
 *
 * Matching is case-insensitive and anywhere in the name, ranked in two
 * passes - what STARTS with the query, then what merely contains it - so
 * "set" puts Settings first without a score whose behaviour nobody can
 * predict.  An empty query matches everything, which is what fills the
 * panel when it first opens.
 */
enum taskbar_status taskbar_set_search_open(bool open,
    struct ui_rect *damage);
bool taskbar_search_panel_open(void);
struct ui_rect taskbar_search_panel_bounds(void);
/* The query as typed, "" when empty; never NULL. */
const char *taskbar_search_query(void);
size_t taskbar_search_result_count(void);

/*
 * The backdrop the acrylic material samples.  The caller draws the desktop
 * first, then hands the taskbar the finished picture; the taskbar copies the
 * strip under itself before it lays any glass down, so the blur is of the
 * real desktop rather than of a guess about it.
 */
enum taskbar_status taskbar_capture_backdrop(void);

enum taskbar_status taskbar_draw(struct ui_rect damage);

/*
 * Input.  Each returns the rectangle that must be redrawn, which is empty
 * when nothing changed, and any action the click asked for.
 */
enum taskbar_status taskbar_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum taskbar_status taskbar_pointer_press(struct ui_point point,
    enum ui_pointer_button button, struct ui_rect *damage);
enum taskbar_status taskbar_pointer_release(struct ui_point point,
    enum ui_pointer_button button, struct ui_rect *damage,
    struct taskbar_action *action);
enum taskbar_status taskbar_pointer_leave(struct ui_rect *damage);
/*
 * Typing, which only the search panel has any use for - a keystroke with it
 * shut is dropped rather than opening it, since this shell has no focus
 * model that could have handed the bar the keyboard.  Backspace deletes;
 * Enter runs the BEST MATCH and closes the panel, reporting it through
 * `action` the way a click on that row would have.
 */
enum taskbar_status taskbar_text_input(char character, struct ui_rect *damage);
enum taskbar_status taskbar_key_backspace(struct ui_rect *damage);
/*
 * Alt+F4, which opens Task Manager.
 *
 * Windows binds Ctrl+Shift+Esc to it and gives Alt+F4 to "close the
 * foreground window"; this shell has no window manager to close anything
 * with, so the chord is free and it is bound to the one diagnostic that
 * has no button anywhere.  Reports TASKBAR_ACTION_TASK_MANAGER and closes
 * whatever panel was open, since a panel standing over the window you just
 * asked for would be in the way of it.
 */
enum taskbar_status taskbar_key_alt_f4(struct ui_rect *damage,
    struct taskbar_action *action);
enum taskbar_status taskbar_key_enter(struct ui_rect *damage,
    struct taskbar_action *action);
enum taskbar_status taskbar_dismiss(struct ui_rect *damage);
bool taskbar_contains(struct ui_point point);
bool taskbar_flyout_open(void);
struct ui_rect taskbar_flyout_bounds(void);

/*
 * Advance every running animation to the monotonic clock and report what
 * that moved.  Returns true while another frame is still owed.
 */
bool taskbar_animate(struct ui_rect *damage);
bool taskbar_animating(void);

struct taskbar_counters taskbar_get_counters(void);

/*
 * Measure the live layout and refuse it if it has drifted from the values in
 * the implementation's METRICS block.  Returns the measurements either way,
 * so a caller can print what it found alongside why it was rejected.
 */
enum taskbar_status taskbar_verify_installed(struct taskbar_proof *proof);
const char *taskbar_installed_proof_failure(void);

bool taskbar_self_test(void);
const char *taskbar_self_test_failure(void);
const char *taskbar_status_string(enum taskbar_status status);

#endif
