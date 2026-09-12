/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * File Explorer.  See include/phipia/explorer.h for the shape and for what
 * Phipia does differently.
 */

#include <phipia/explorer.h>

#include <phipia/clock.h>
#include <phipia/cursor.h>
#include <phipia/framebuffer.h>
#include <phipia/ui_font.h>

#include "explorer_art.h"
#include "explorer_glyphs.h"
#include "shell_icons.h"
#include "ui_motion.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION, all of them: read off a Windows 10 Explorer window at
 * 100% scaling rather than taken from a published resource.  The BANDS are
 * Windows'; the pixel sizes are this file's reading of them.
 */

#define EXPLORER_BORDER 1U
#define EXPLORER_CAPTION 32U
#define EXPLORER_CAPTION_BUTTON 46U
/* The Quick Access Toolbar, which sits INSIDE the caption at its left end. */
#define EXPLORER_QAT_ICON 16U
#define EXPLORER_QAT_SLOT 22U
#define EXPLORER_QAT_INSET 6U
/*
 * THE COMMAND BAR, which is where this window stops copying Windows 10 and
 * starts being usable.
 *
 * Windows 10 puts a four-tab strip over a five-group ribbon here: about
 * twenty buttons and a hundred and twenty-four pixels of chrome, of which a
 * copy could make perhaps four buttons actually do anything.  The rest were
 * pictures of buttons, which is the one thing this project has tried not to
 * ship.
 *
 * This is one row instead: New, the five things you do to a selected file,
 * and the two that change how the list is shown.  Eight controls, forty
 * pixels, and all eight of them do the thing they are drawn as.  The window
 * keeps Windows 10's chrome, its address bar, its navigation pane, its
 * details columns and its status bar; it loses the ribbon, which is the
 * part nobody was using.
 */
#define EXPLORER_COMMANDS 40U
#define EXPLORER_COMMAND_ICON 20U
#define EXPLORER_COMMAND_PAD 10U
#define EXPLORER_COMMAND_GAP 6U
#define EXPLORER_COMMAND_SEPARATOR 9U
#define EXPLORER_ADDRESS 34U
#define EXPLORER_ADDRESS_SLOT 30U    /* back, forward and up each get one */
/* Windows pins the search box to the right end of the address bar at a
 * fixed width and gives the breadcrumb what is left over. */
#define EXPLORER_SEARCH_WIDTH 200U
/* How close to a window's edge counts as reaching for it rather than for
 * what is inside it.  Windows 10's own frame is about this wide. */
#define EXPLORER_RESIZE_EDGE 4U
#define EXPLORER_NAV 200U
#define EXPLORER_HEADER 26U
/*
 * Windows 10's details row is twenty-two pixels with a sixteen-pixel icon,
 * which is tight enough that a file type icon becomes a grey smudge and the
 * one thing it is there to tell you is lost.  Two more pixels of row and
 * four more of icon costs a screen about one row in twelve and is the
 * difference between reading the list and squinting at it.
 */
#define EXPLORER_ROW 24U
#define EXPLORER_STATUS 24U
#define EXPLORER_TREE_ROW 28U
#define EXPLORER_TREE_INDENT 20U
#define EXPLORER_TREE_ICON 20U
/*
 * The expander chevron's slot.
 *
 * Sixteen and not twelve: the smallest cell the mark exists at IS sixteen,
 * and a twelve-pixel box does not shrink it - it draws sixteen pixels
 * starting at the box's left edge and bleeds four into whatever is beside
 * it.  Every box in this window is at least as wide as the mark it holds,
 * for that reason.
 */
#define EXPLORER_TREE_MARK 16U
#define EXPLORER_PAD 8U
/*
 * The narrowest window the command bar's left group fits in.
 *
 * Windows 10's five ribbon groups came to about eleven hundred pixels, which
 * is why it starts folding them into dropdowns on anything narrower.  Six
 * controls come to under four hundred, so the bar simply fits - and the
 * self-test holds it to that rather than letting it grow back.
 */
#define EXPLORER_COMMANDS_MIN_WIDTH 640U
/* The row of column headings, and where each column starts inside the list. */
#define EXPLORER_COLUMN_NAME 0U
#define EXPLORER_COLUMN_MODIFIED 300U
#define EXPLORER_COLUMN_TYPE 460U
#define EXPLORER_COLUMN_SIZE 610U
#define EXPLORER_ROW_ICON 20U
/* The sort mark beside whichever heading the list is ordered by. */
#define EXPLORER_SORT_MARK 16U
/*
 * TILES, the second of the two view modes.
 *
 * Windows 10 offers eight - extra large, large, medium and small icons,
 * list, details, tiles, content - which is seven more decisions than anyone
 * makes twice.  Two is the number that earns its place: a table when you
 * want the columns, and big icons when you are looking at pictures.  The
 * icon is 48, which is Windows' own "Medium icons", and the artwork exists
 * at that size so it is drawn one to one rather than blown up.
 */
#define EXPLORER_TILE_WIDTH 120U
#define EXPLORER_TILE_HEIGHT 92U
#define EXPLORER_TILE_ICON 48U
#define EXPLORER_TILE_PAD 10U
/* Phipia's accent bar on a selected row. */
#define EXPLORER_SELECT_BAR 3U

/* ================================================================ PALETTE
 *
 * Windows 10's Explorer is a light window whatever the taskbar is doing:
 * a white list and near-white chrome.  It gained no dark theme.
 * PENDING VERIFICATION.
 */
struct explorer_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define EXPLORER_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct explorer_rgb caption_fill = EXPLORER_RGB(0xFFU, 0xFFU,
    0xFFU);
static const struct explorer_rgb chrome = EXPLORER_RGB(0xF0U, 0xF0U, 0xF0U);
static const struct explorer_rgb list_fill = EXPLORER_RGB(0xFFU, 0xFFU,
    0xFFU);
static const struct explorer_rgb rule = EXPLORER_RGB(0xDCU, 0xDCU, 0xDCU);
static const struct explorer_rgb rule_soft = EXPLORER_RGB(0xEAU, 0xEAU,
    0xEAU);
static const struct explorer_rgb ink = EXPLORER_RGB(0x1AU, 0x1AU, 0x1AU);
static const struct explorer_rgb ink_soft = EXPLORER_RGB(0x66U, 0x66U, 0x66U);
static const struct explorer_rgb ink_faint = EXPLORER_RGB(0x8CU, 0x8CU,
    0x8CU);
static const struct explorer_rgb accent = EXPLORER_RGB(0x00U, 0x78U, 0xD7U);
static const struct explorer_rgb select_fill = EXPLORER_RGB(0xE3U, 0xF0U,
    0xFBU);
static const struct explorer_rgb hover_fill = EXPLORER_RGB(0xF2U, 0xF7U,
    0xFCU);
static const struct explorer_rgb field_fill = EXPLORER_RGB(0xFFU, 0xFFU,
    0xFFU);
static const struct explorer_rgb border_active = EXPLORER_RGB(0x00U, 0x78U,
    0xD7U);
static const struct explorer_rgb border_inactive = EXPLORER_RGB(0x9BU, 0x9BU,
    0x9BU);

/*
 * The type colours, which is where this stops being a copy.
 *
 * Windows 10 draws every one of these marks in the same grey, so a folder of
 * two hundred files is two hundred identical shapes and the only way to find
 * the picture among them is to read the Type column.  Giving each kind its
 * own hue costs nothing and is the single largest usability difference
 * between this window and the one it copies.
 */
static const struct explorer_rgb kind_colour[EXPLORER_KIND_COUNT] = {
    EXPLORER_RGB(0xE8U, 0xB1U, 0x2CU),   /* folder,  the pad yellow    */
    EXPLORER_RGB(0x54U, 0x6EU, 0x8AU),   /* text,    slate             */
    EXPLORER_RGB(0x11U, 0x9DU, 0xA4U),   /* image,   teal              */
    EXPLORER_RGB(0x7BU, 0x3FU, 0xC4U),   /* audio,   violet            */
    EXPLORER_RGB(0xD1U, 0x5B, 0x1FU),    /* video,   burnt orange      */
    EXPLORER_RGB(0xC0U, 0x8AU, 0x0EU),   /* archive, amber             */
    EXPLORER_RGB(0x2AU, 0x8AU, 0x4AU),   /* code,    green             */
    EXPLORER_RGB(0x7AU, 0x7AU, 0x7AU)    /* generic, grey              */
};

/*
 * The icon each kind draws with.  These are the illustrations, not the line
 * marks: kind_colour above is what a LINE mark would be tinted by, and is
 * chosen to agree with the colour inside each illustration.
 *
 * A folder draws the artwork rather than a rasterized SVG.  The two are the
 * same shape at a glance and the artwork is the one that reads as a folder
 * at sixteen pixels, which is the size the list spends most of its life at.
 */
static const char *const kind_glyph[EXPLORER_KIND_COUNT] = {
    "folder-yellow", "file-text", "file-image", "file-audio", "file-video",
    "file-archive", "file-code", "file-generic"
};

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool focused = true;
/*
 * What the command bar needs to know.
 *
 * The clipboard is one slot, which is what a shell with no system clipboard
 * behind it can honestly offer: Copy remembers a row, Cut remembers it and
 * marks it to be removed, and Paste puts a copy of it into the list under a
 * name nothing else is using.  Nothing leaves this window - there is no
 * other window in this shell to hand a file to yet, and pretending there
 * was would make Copy a lie rather than a limitation.
 */
static bool clipboard_full;
static bool clipboard_cut;
static struct explorer_item clipboard_item;
static size_t clipboard_source = (size_t)-1;

/* Which column the list is ordered by, and which way.  Windows sorts by
 * clicking a column heading; this adds a Sort menu that does the same
 * thing, because a heading is a small target and not an obvious one.
 * enum explorer_sort is in the header - a caller can set the order. */
static enum explorer_sort sort_column = EXPLORER_SORT_NAME;
static bool sort_descending;

/* Details or tiles.  Windows 10 offers eight view modes; two is the number
 * that earns its place - a table when you want the columns, and big icons
 * when you are looking at pictures. */
static bool tiles_view;

static size_t hover_command = (size_t)-1;
/* Which breadcrumb segment the pointer is over, and which of the address
 * bar's three arrows - both only to underline or light one, which is the
 * whole of how you find out they are buttons. */
static size_t hover_crumb = (size_t)-1;
static size_t hover_address = (size_t)-1;
/*
 * Which flyout is open, as the index of the control that owns it, and which
 * of its rows the pointer is on.
 *
 * One menu machine rather than three: New, Sort and View differ in their
 * rows and in nothing else, and an earlier version of this file carried the
 * New menu's open flag, hover index, geometry, drawing, hit-testing and
 * dismissal as its own copy of all six.  Two more menus would have been two
 * more copies.
 */
static size_t open_menu = (size_t)-1;
static size_t menu_hover = (size_t)-1;
/*
 * Where the navigation buttons have been.
 *
 * Back, forward and up were three drawn arrows that did nothing at all -
 * the address bar's share of the "pictures of buttons" this rework is
 * mostly about.  What this window can honestly navigate is its own
 * navigation pane, so that is what they walk: every place selected goes on
 * the stack, Back and Forward move through it, and Up jumps to the depth-0
 * root above wherever you are standing.  The file list does not follow,
 * because the file list is the caller's - and a button that greys out when
 * it has nowhere to go says so without being lied to.
 */
#define EXPLORER_HISTORY 16U
static size_t history[EXPLORER_HISTORY];
static size_t history_count;
static size_t history_at;
static char window_title[EXPLORER_NAME_BYTES] = "Documents";
static struct explorer_item items[EXPLORER_MAX_ITEMS];
static struct explorer_place places[EXPLORER_MAX_PLACES];
static char crumbs[EXPLORER_MAX_CRUMBS][EXPLORER_NAME_BYTES];
static size_t hover_item = (size_t)-1;
static size_t hover_place = (size_t)-1;
/*
 * What is being LEFT, and the fades.
 *
 * Windows cross-fades a hovered row rather than switching it, which is what
 * makes running the pointer down a long file list read as one movement
 * instead of a strobe.  The list and the sidebar are hovered independently,
 * so each gets its own pair; one motion per row would be forty-four of them
 * for a state only two rows in each can ever be in.
 */
static size_t leaving_item = (size_t)-1;
static size_t leaving_place = (size_t)-1;
static struct ui_motion item_fade;
static struct ui_motion item_leave_fade;
static struct ui_motion place_fade;
static struct ui_motion place_leave_fade;

/*
 * The rename in progress, if any.
 *
 * A file's editable STEM and its fixed suffix are kept apart rather than as
 * one buffer with a remembered split point: Windows never lets the initial
 * selection or a keystroke touch the extension, and two strings make that
 * true by construction instead of by a boundary check on every edit.
 */
static bool rename_active;
static size_t rename_index = (size_t)-1;
static char rename_stem[EXPLORER_NAME_BYTES];
static char rename_suffix[EXPLORER_FIELD_BYTES];
static size_t rename_length;    /* bytes in rename_stem, not counting the NUL */
/* True until the first keystroke narrows it - the "name highlighted, type
 * to replace" state Windows opens a new item in.  Only explorer_create_item()
 * opens a rename at present, so Escape always deletes the row rather than
 * choosing between deleting and reverting; that is this feature's actual
 * scope; an F2 rename of an existing row would need a revert path and does
 * not exist yet to need one. */
static bool rename_selected;
/*
 * Whether the row being renamed was made by the rename - which decides what
 * Escape means.  Windows deletes a New folder abandoned before it is named
 * and puts the old name back on one you were renaming, and a version of
 * this file that only ever opened a rename from Create could not tell the
 * two apart.  Rename on the command bar can, so it has to.
 */
static bool rename_created;
static char rename_original[EXPLORER_NAME_BYTES];
static struct explorer_action pending_action;
static bool action_waiting;
static bool caret_visible = true;
static uint32_t caret_phase;    /* which half-second the last draw used */

static void copy_field(char *destination, const char *source, size_t bytes);

static void queue_action(enum explorer_action_kind kind,
    enum explorer_kind item_kind, const char *source, const char *destination)
{
    pending_action = (struct explorer_action){
        .kind = kind,
        .item_kind = item_kind
    };
    copy_field(pending_action.source, source,
        sizeof(pending_action.source));
    copy_field(pending_action.destination, destination,
        sizeof(pending_action.destination));
    action_waiting = true;
}

/*
 * The command palette: Ctrl+K.  See explorer_toggle_command_palette() in
 * the header for what it is and why it exists.  caret_visible/caret_phase
 * above are shared with it rather than duplicated - the palette and a
 * rename cannot be open at once, so the one blinking caret this window ever
 * needs is never asked to mean two things at the same time.
 */
enum palette_action {
    PALETTE_NEW_FOLDER = 0,
    PALETTE_NEW_TEXT,
    PALETTE_NEW_BITMAP,
    PALETTE_SELECT_ALL,
    PALETTE_SELECT_NONE,
    PALETTE_INVERT_SELECTION,
    PALETTE_TOGGLE_VIEW,
    PALETTE_CLEAR_SEARCH,
    PALETTE_GO_TO_PLACE
};

struct palette_command {
    char label[EXPLORER_NAME_BYTES];
    const char *glyph;
    enum palette_action action;
    size_t place_index;    /* only meaningful for PALETTE_GO_TO_PLACE */
};

/* Seven fixed commands plus one "Go to" per place the tree can hold.  The
 * seventh - "Clear search" - is only offered while there is a search to
 * clear, so the list is usually one shorter than it has room for. */
#define PALETTE_CAPACITY (7U + EXPLORER_MAX_PLACES)

static bool palette_open;
static char palette_query[EXPLORER_FIELD_BYTES];
static size_t palette_query_length;
static struct palette_command palette_all[PALETTE_CAPACITY];
static size_t palette_all_count;
/* Indices into palette_all, in match order - startswith matches before
 * plain substring matches, and index [0] is what Enter runs. */
static size_t palette_matches[PALETTE_CAPACITY];
static size_t palette_match_count;
static size_t palette_hover = (size_t)-1;

/*
 * The search box, which filters the list rather than decorating the address
 * bar.  Windows 10 searches the FOLDER - it walks what is on disk under
 * where you are standing and shows matches from below it too, with a
 * progress bar along the address bar while it works.  This filters the rows
 * this window was handed, which is the honest version of that for a shell
 * whose file list is set by explorer_set_item() rather than read off a
 * volume: everything it could match is already on screen, so there is
 * nothing to walk and nothing to make you wait for.
 *
 * A filtered row is not hidden by drawing around it - item_visible() takes
 * it out of the layout the way place_visible() takes a collapsed branch's
 * children out of the tree, so hit-testing, selection and the row count
 * agree with what is on screen without any of them being taught about
 * searching.
 */
static bool search_focused;
static char search_query[EXPLORER_FIELD_BYTES];
static size_t search_query_length;
/* Whether the pointer is over the clear mark, which only exists while
 * there is something to clear. */
static bool search_clear_hot;

static const char *self_test_failure = "explorer self-test has not run";

const char *explorer_status_string(enum explorer_status status)
{
    switch (status) {
    case EXPLORER_STATUS_OK:
        return "ok";
    case EXPLORER_STATUS_NULL_ARGUMENT:
        return "null argument";
    case EXPLORER_STATUS_NOT_INITIALIZED:
        return "explorer not initialized";
    case EXPLORER_STATUS_BAD_INDEX:
        return "explorer index is out of range";
    case EXPLORER_STATUS_UNSUPPORTED_GEOMETRY:
        return "explorer geometry is unsupported";
    case EXPLORER_STATUS_SURFACE_FAILURE:
        return "explorer surface refused a pixel";
    default:
        return "unknown explorer status";
    }
}

/* ============================================================== PRIMITIVES */

static uint32_t pack_rgb(struct explorer_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return ((uint32_t)colour.red << format.red_position) |
        ((uint32_t)colour.green << format.green_position) |
        ((uint32_t)colour.blue << format.blue_position);
}

static struct ui_rect intersect(struct ui_rect left, struct ui_rect right)
{
    const uint32_t x0 = left.x > right.x ? left.x : right.x;
    const uint32_t y0 = left.y > right.y ? left.y : right.y;
    const uint32_t x1a = left.x + left.width;
    const uint32_t x1b = right.x + right.width;
    const uint32_t y1a = left.y + left.height;
    const uint32_t y1b = right.y + right.height;
    const uint32_t x1 = x1a < x1b ? x1a : x1b;
    const uint32_t y1 = y1a < y1b ? y1a : y1b;

    if (x1 <= x0 || y1 <= y0) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ x0, y0, x1 - x0, y1 - y0 };
}

static bool holds(struct ui_rect area, struct ui_point point)
{
    return point.x >= 0 && point.y >= 0 &&
        (uint32_t)point.x >= area.x &&
        (uint32_t)point.x < area.x + area.width &&
        (uint32_t)point.y >= area.y &&
        (uint32_t)point.y < area.y + area.height;
}

static enum explorer_status fill(struct ui_rect area, struct ui_rect damage,
    struct explorer_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return EXPLORER_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return EXPLORER_STATUS_OK;
}

static enum explorer_status outline(struct ui_rect area,
    struct ui_rect damage, struct explorer_rgb colour)
{
    enum explorer_status status = fill((struct ui_rect){ area.x, area.y,
        area.width, 1U }, damage, colour);

    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y + area.height - 1U,
            area.width, 1U }, damage, colour);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y, 1U, area.height },
            damage, colour);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ area.x + area.width - 1U, area.y,
            1U, area.height }, damage, colour);
    }
    return status;
}
/* The same, mixed into what is already there - which is what a cross-fade
 * needs and a plain fill cannot do. */
static enum explorer_status blend(struct ui_rect area, struct ui_rect damage,
    struct explorer_rgb colour, uint32_t alpha)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t over = pack_rgb(colour);

    if (alpha == 0U) {
        return EXPLORER_STATUS_OK;
    }
    if (alpha >= 255U) {
        return fill(area, damage, colour);
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return EXPLORER_STATUS_SURFACE_FAILURE;
            }
            red = (((over >> format.red_position) & 0xFFU) * alpha +
                ((under >> format.red_position) & 0xFFU) * (255U - alpha) +
                127U) / 255U;
            green = (((over >> format.green_position) & 0xFFU) * alpha +
                ((under >> format.green_position) & 0xFFU) * (255U - alpha) +
                127U) / 255U;
            blue = (((over >> format.blue_position) & 0xFFU) * alpha +
                ((under >> format.blue_position) & 0xFFU) * (255U - alpha) +
                127U) / 255U;
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) != SURFACE_STATUS_OK) {
                return EXPLORER_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return EXPLORER_STATUS_OK;
}

static struct ui_rect join(struct ui_rect left, struct ui_rect right)
{
    uint32_t x;
    uint32_t y;
    uint32_t r;
    uint32_t b;

    if (left.width == 0U || left.height == 0U) {
        return right;
    }
    if (right.width == 0U || right.height == 0U) {
        return left;
    }
    x = left.x < right.x ? left.x : right.x;
    y = left.y < right.y ? left.y : right.y;
    r = left.x + left.width > right.x + right.width ?
        left.x + left.width : right.x + right.width;
    b = left.y + left.height > right.y + right.height ?
        left.y + left.height : right.y + right.height;
    return (struct ui_rect){ x, y, r - x, b - y };
}


static enum explorer_status text_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct explorer_rgb colour)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (clip.width == 0U || clip.height == 0U || body == NULL) {
        return EXPLORER_STATUS_OK;
    }
    bounds.x = window_rect.x;
    bounds.y = window_rect.y;
    bounds.width = window_rect.width;
    bounds.height = window_rect.height;
    region.x = clip.x;
    region.y = clip.y;
    region.width = clip.width;
    region.height = clip.height;
    (void)ui_font_draw_text_clipped(canvas, bounds, region, x, baseline, body,
        pack_rgb(colour), NULL);
    return EXPLORER_STATUS_OK;
}

/* Where a label starts if it is to be centred in a slot - and the slot's own
 * left edge when the label is wider than the slot, because the unsigned
 * arithmetic otherwise wraps and puts it off the window entirely. */
static uint32_t centred_x(uint32_t left, uint32_t width, uint32_t text_width)
{
    return width > text_width ? left + (width - text_width) / 2U : left;
}

static uint32_t width_of(const char *body)
{
    uint32_t width = 0U;

    if (body == NULL || ui_font_text_width(body, &width) !=
            UI_FONT_STATUS_OK) {
        return 0U;
    }
    return width;
}

/*
 * Text that ends in an ellipsis rather than at a cliff.
 *
 * text_column() below clips to the column, which guarantees one column
 * never draws over the next and says nothing at all about the fact that it
 * cut something off: "Compressed Folder" arrived as "Compressed Fo" and
 * "taskbar-dark.png" under a tile as "taskbar-dark.p", both of which read
 * as a bug rather than as a name too long for the space.  Windows puts
 * three dots there.  This drops characters until the name and the dots fit,
 * which costs one width measurement per character dropped and happens only
 * for the names that actually overflow.
 */
static enum explorer_status text_elided(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct explorer_rgb colour,
    uint32_t limit)
{
    char shortened[EXPLORER_NAME_BYTES + 4U];
    size_t length = 0U;

    if (body == NULL) {
        return EXPLORER_STATUS_OK;
    }
    if (width_of(body) <= limit) {
        return text_at(damage, x, baseline, body, colour);
    }
    while (body[length] != '\0' && length + 4U < sizeof(shortened)) {
        shortened[length] = body[length];
        ++length;
    }
    while (length > 0U) {
        --length;
        shortened[length] = '.';
        shortened[length + 1U] = '.';
        shortened[length + 2U] = '.';
        shortened[length + 3U] = '\0';
        if (width_of(shortened) <= limit) {
            break;
        }
    }
    /* Below about three characters there is nothing left to elide to;
     * clipping is then the only honest answer and text_at's own clip does
     * it. */
    return text_at(intersect(damage, (struct ui_rect){ x, window_rect.y,
        limit, window_rect.height }), x, baseline, shortened, colour);
}

static bool names_match(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static size_t append_literal(char *into, size_t capacity, size_t at,
    const char *text)
{
    while (*text != '\0' && at + 1U < capacity) {
        into[at++] = *text++;
    }
    into[at] = '\0';
    return at;
}

static size_t append_uint(char *into, size_t capacity, size_t at,
    uint32_t value)
{
    char scratch[10];
    size_t length = 0U;

    do {
        scratch[length++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    } while (value != 0U && length < sizeof(scratch));
    while (length > 0U && at + 1U < capacity) {
        into[at++] = scratch[--length];
    }
    into[at] = '\0';
    return at;
}

static void copy_field(char *destination, const char *source, size_t bytes)
{
    size_t index = 0U;

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    while (index + 1U < bytes && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static const uint8_t *glyph_cell(const char *name, uint32_t wanted,
    uint32_t *size)
{
    for (size_t index = 0U; index < EXPLORER_LUCIDE_COUNT; ++index) {
        /*
         * The LARGEST cell that fits, not the smallest that covers.  These
         * are composited one to one rather than resampled, so a cell bigger
         * than its box does not shrink - it overflows, and the centring
         * arithmetic underflows and throws the mark off the window.  That is
         * what swallowed the address bar's chevrons and its refresh.
         */
        size_t choice = 0U;

        if (!names_match(explorer_lucide[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < EXPLORER_LUCIDE_SIZES; ++option) {
            if (explorer_lucide_size[option] <= wanted) {
                choice = option;
            }
        }
        *size = explorer_lucide_size[choice];
        return explorer_lucide[index].alpha[choice];
    }
    return NULL;
}

/*
 * An illustration, from either of the two tables that hold one.
 *
 * Windows' file and place icons are little pictures - a white page with a
 * folded corner, a two-tone folder, a monitor with a blue screen - not
 * hairline outlines in one ink.  Drawing them as line art is what makes a
 * copy of File Explorer read as a Linux icon theme, so these come from
 * their own tables and keep their own colours.
 *
 * GHOST is coverage in 255ths, and exists for one thing: a row that has
 * been Cut is still there and is on its way out, which is what Windows says
 * by drawing it at about half strength.  Everything else passes 255.
 */
static void draw_bitmap(const uint32_t *pixels, const uint8_t *plane,
    uint32_t size, struct ui_rect box, struct ui_rect damage, uint32_t ghost)
{
    const struct framebuffer_state format = framebuffer_get_state();
    /* Centred and drawn one to one - never resampled.  A cell bigger than
     * its box does not shrink, it overflows, and the centring arithmetic
     * underflows and throws the mark off the window; the callers below pick
     * the largest cell that FITS for exactly that reason. */
    const struct ui_rect placed = {
        box.x + (box.width > size ? (box.width - size) / 2U : 0U),
        box.y + (box.height > size ? (box.height - size) / 2U : 0U),
        size, size };
    const struct ui_rect clipped = intersect(placed, damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - placed.x + x;
            const size_t offset = (size_t)local_y * size + local_x;
            const uint32_t coverage = ((uint32_t)plane[offset] * ghost +
                127U) / 255U;
            const uint32_t stored = pixels[offset];
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (coverage == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return;
            }
            /* The planes are baked 0x00RRGGBB; recompose into whatever
             * order this device actually keeps its channels in. */
            red = (((stored >> 16) & 0xFFU) * coverage +
                ((under >> format.red_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            green = (((stored >> 8) & 0xFFU) * coverage +
                ((under >> format.green_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            blue = ((stored & 0xFFU) * coverage +
                ((under >> format.blue_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) != SURFACE_STATUS_OK) {
                return;
            }
        }
    }
}

/*
 * The two illustration tables.  shell_icons.h is rasterized from filled
 * SVGs; explorer_art.h is bitmap artwork carried as it was drawn - the
 * folders and the drive - reduced from the original at each size rather
 * than from a larger cell.  A name in either draws; a name in neither falls
 * through to the line glyphs the chrome uses.
 */
static bool draw_art(const char *name, struct ui_rect box,
    struct ui_rect damage, uint32_t ghost)
{
    const uint32_t wanted = box.width < box.height ? box.width : box.height;

    for (size_t index = 0U; index < EXPLORER_ART_COUNT; ++index) {
        size_t choice = 0U;

        if (!names_match(explorer_art[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < EXPLORER_ART_SIZES; ++option) {
            if (explorer_art_size[option] <= wanted) {
                choice = option;
            }
        }
        draw_bitmap(explorer_art[index].pixels[choice],
            explorer_art[index].alpha[choice], explorer_art_size[choice],
            box, damage, ghost);
        return true;
    }
    return false;
}

static bool draw_shell_icon(const char *name, struct ui_rect box,
    struct ui_rect damage, uint32_t ghost)
{
    const uint32_t wanted = box.width < box.height ? box.width : box.height;

    for (size_t index = 0U; index < SHELL_ICON_COUNT; ++index) {
        size_t choice = 0U;

        if (!names_match(shell_icons[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < SHELL_ICON_SIZES; ++option) {
            if (shell_icon_size[option] <= wanted) {
                choice = option;
            }
        }
        draw_bitmap(shell_icons[index].pixels[choice],
            shell_icons[index].alpha[choice], shell_icon_size[choice],
            box, damage, ghost);
        return true;
    }
    return false;
}

/* Whether a name draws anything at all, from any of the three tables.  The
 * self-test asks this rather than the line table alone, so a mark that
 * moved to an illustration set does not read as missing. */
static bool has_icon(const char *name, uint32_t wanted)
{
    uint32_t size = 0U;

    for (size_t index = 0U; index < EXPLORER_ART_COUNT; ++index) {
        if (names_match(explorer_art[index].name, name)) {
            return true;
        }
    }
    for (size_t index = 0U; index < SHELL_ICON_COUNT; ++index) {
        if (names_match(shell_icons[index].name, name)) {
            return true;
        }
    }
    return glyph_cell(name, wanted, &size) != NULL;
}

/* A mark, from whichever table holds it: artwork and shell icons keep their
 * own colours, a line glyph is tinted by the caller. */
static enum explorer_status draw_glyph_ghosted(const char *name,
    struct ui_rect box, struct ui_rect damage, struct explorer_rgb colour,
    uint32_t ghost)
{
    uint32_t size = 0U;
    const uint8_t *cell;
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t over = pack_rgb(colour);
    struct ui_rect placed;
    struct ui_rect clipped;

    if (draw_art(name, box, damage, ghost)) {
        return EXPLORER_STATUS_OK;
    }
    if (draw_shell_icon(name, box, damage, ghost)) {
        return EXPLORER_STATUS_OK;
    }
    cell = glyph_cell(name, box.width < box.height ? box.width : box.height,
        &size);
    if (cell == NULL) {
        return EXPLORER_STATUS_OK;
    }
    placed = (struct ui_rect){
        box.x + (box.width > size ? (box.width - size) / 2U : 0U),
        box.y + (box.height > size ? (box.height - size) / 2U : 0U),
        size, size };
    clipped = intersect(placed, damage);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - placed.x + x;
            const uint32_t coverage =
                ((uint32_t)cell[local_y * size + local_x] * ghost + 127U) /
                255U;
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (coverage == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return EXPLORER_STATUS_SURFACE_FAILURE;
            }
            red = (((over >> format.red_position) & 0xFFU) * coverage +
                ((under >> format.red_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            green = (((over >> format.green_position) & 0xFFU) * coverage +
                ((under >> format.green_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            blue = (((over >> format.blue_position) & 0xFFU) * coverage +
                ((under >> format.blue_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) != SURFACE_STATUS_OK) {
                return EXPLORER_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return EXPLORER_STATUS_OK;
}

static enum explorer_status draw_glyph(const char *name, struct ui_rect box,
    struct ui_rect damage, struct explorer_rgb colour)
{
    return draw_glyph_ghosted(name, box, damage, colour, 255U);
}

/* ================================================================ GEOMETRY */

static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){ window_rect.x + EXPLORER_BORDER,
        window_rect.y + EXPLORER_BORDER,
        window_rect.width - EXPLORER_BORDER * 2U, EXPLORER_CAPTION };
}

static struct ui_rect commands_rect(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x, caption.y + caption.height,
        caption.width, EXPLORER_COMMANDS };
}

static struct ui_rect address_rect(void)
{
    const struct ui_rect bar = commands_rect();

    return (struct ui_rect){ bar.x, bar.y + bar.height, bar.width,
        EXPLORER_ADDRESS };
}

/*
 * The address bar's two fields.  Windows 10 pins the search box to the
 * right end at a fixed width and gives the breadcrumb whatever is left,
 * which is why a narrow window loses breadcrumb rather than search box.
 *
 * Both were worked out inline inside draw_address() before the search box
 * did anything, because drawing was the only thing that needed to know
 * where they were.  A box that can be clicked and typed into needs its
 * geometry in one place instead of two.
 */
static struct ui_rect address_field_rect(void)
{
    const struct ui_rect band = address_rect();
    /* Back, forward and up, at EXPLORER_ADDRESS_SLOT each. */
    const uint32_t pen = band.x + 4U + 3U * EXPLORER_ADDRESS_SLOT;

    return (struct ui_rect){ pen + 4U, band.y + 4U,
        band.width - (pen + 4U - band.x) - EXPLORER_SEARCH_WIDTH - 12U,
        band.height - 8U };
}

/* Back, forward and up, in that order at the bar's left end. */
static struct ui_rect address_slot_rect(size_t index)
{
    const struct ui_rect band = address_rect();

    if (index >= 3U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){
        band.x + 4U + (uint32_t)index * EXPLORER_ADDRESS_SLOT, band.y,
        EXPLORER_ADDRESS_SLOT, band.height };
}

static struct ui_rect search_rect(void)
{
    const struct ui_rect field = address_field_rect();

    return (struct ui_rect){ field.x + field.width + 4U, field.y,
        EXPLORER_SEARCH_WIDTH, field.height };
}

/*
 * Where breadcrumb segment `index` sits, or an empty rect if it is not
 * there.  Walks the same pen draw_address() walks - segment, four pixels,
 * and a sixteen-pixel chevron between neighbours - because a link you can
 * click has to be measured exactly where it was drawn.
 */
static struct ui_rect crumb_rect(size_t index)
{
    const struct ui_rect field = address_field_rect();
    uint32_t pen = field.x + 8U;
    size_t last = 0U;

    if (index >= EXPLORER_MAX_CRUMBS || crumbs[index][0] == '\0') {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    for (size_t scan = 0U; scan < EXPLORER_MAX_CRUMBS; ++scan) {
        if (crumbs[scan][0] != '\0') {
            last = scan;
        }
    }
    for (size_t scan = 0U; scan < EXPLORER_MAX_CRUMBS; ++scan) {
        uint32_t width;

        if (crumbs[scan][0] == '\0') {
            continue;
        }
        width = width_of(crumbs[scan]);
        if (scan == index) {
            return (struct ui_rect){ pen, field.y, width, field.height };
        }
        pen += width + 4U;
        if (scan != last) {
            pen += 16U;
        }
    }
    return (struct ui_rect){ 0U, 0U, 0U, 0U };
}

/*
 * The clear mark at the search box's right end, which exists only while
 * there is a query to clear - Windows puts an x there and turns it back
 * into a magnifier once the box is empty, so the mark is never offering to
 * undo nothing.
 */
static struct ui_rect search_clear_rect(void)
{
    const struct ui_rect box = search_rect();

    if (search_query[0] == '\0') {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ box.x + box.width - 24U, box.y, 20U,
        box.height };
}

static struct ui_rect status_rect(void)
{
    return (struct ui_rect){ window_rect.x + EXPLORER_BORDER,
        window_rect.y + window_rect.height - EXPLORER_BORDER -
            EXPLORER_STATUS,
        window_rect.width - EXPLORER_BORDER * 2U, EXPLORER_STATUS };
}

static struct ui_rect body_rect(void)
{
    const struct ui_rect address = address_rect();
    const struct ui_rect status = status_rect();

    return (struct ui_rect){ address.x, address.y + address.height,
        address.width, status.y - (address.y + address.height) };
}

static struct ui_rect nav_rect(void)
{
    const struct ui_rect body = body_rect();

    return (struct ui_rect){ body.x, body.y, EXPLORER_NAV, body.height };
}

static struct ui_rect list_rect(void)
{
    const struct ui_rect body = body_rect();

    return (struct ui_rect){ body.x + EXPLORER_NAV, body.y,
        body.width - EXPLORER_NAV, body.height };
}

static struct ui_rect caption_button_rect(uint32_t index)
{
    const struct ui_rect caption = caption_rect();
    const uint32_t from_right = 3U - index;

    return (struct ui_rect){
        caption.x + caption.width - from_right * EXPLORER_CAPTION_BUTTON,
        caption.y, EXPLORER_CAPTION_BUTTON, caption.height };
}

/* Which of the present places this index is, counting from the top. */
/*
 * Whether a place actually shows: present, and not a child sitting under a
 * collapsed parent.
 *
 * Windows 10's tree never goes past depth 1, so "under a collapsed parent"
 * only ever means the nearest depth-0 place before this one is collapsed -
 * there is no deeper nesting to walk to find out.  Before this, expandable
 * and expanded existed only to choose which chevron to draw; a place they
 * described as collapsed still drew every child under it, which is a
 * chevron that lies about the state it is showing.
 */
static bool place_visible(size_t index)
{
    if (!places[index].present) {
        return false;
    }
    if (places[index].depth == 0U) {
        return true;
    }
    for (size_t scan = index; scan > 0U; ) {
        --scan;
        if (!places[scan].present || places[scan].depth != 0U) {
            continue;
        }
        return !places[scan].expandable || places[scan].expanded;
    }
    return true;   /* a depth > 0 place with no parent above it to hide it */
}

static size_t place_position(size_t index)
{
    size_t position = 0U;

    for (size_t scan = 0U; scan < index; ++scan) {
        if (place_visible(scan)) {
            ++position;
        }
    }
    return position;
}

static struct ui_rect place_rect(size_t index)
{
    const struct ui_rect nav = nav_rect();
    const uint32_t top = nav.y + EXPLORER_PAD +
        (uint32_t)place_position(index) * EXPLORER_TREE_ROW;

    if (!place_visible(index) || top + EXPLORER_TREE_ROW > nav.y +
            nav.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ nav.x, top, nav.width, EXPLORER_TREE_ROW };
}

/* The chevron's own hit target within an expandable place's row - the same
 * box draw_nav() paints it in, so a click toggles exactly the mark it is
 * pointing at rather than some approximation of where the mark usually is. */
static struct ui_rect place_chevron_rect(size_t index)
{
    const struct ui_rect row = place_rect(index);
    const uint32_t indent = EXPLORER_PAD +
        (uint32_t)places[index].depth * EXPLORER_TREE_INDENT;

    if (row.width == 0U || !places[index].expandable) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ row.x + indent, row.y, EXPLORER_TREE_MARK,
        row.height };
}

/* Which place the tree says you are standing in, or none. */
static size_t current_place(void)
{
    for (size_t index = 0U; index < EXPLORER_MAX_PLACES; ++index) {
        if (places[index].present && places[index].current) {
            return index;
        }
    }
    return (size_t)-1;
}

static bool can_go_back(void)
{
    return history_count > 1U && history_at > 0U;
}

static bool can_go_forward(void)
{
    return history_count > 0U && history_at + 1U < history_count;
}

/* The depth-0 place above this one, or none when you are already at a root
 * - which is what makes Up grey out at the top of the tree instead of
 * pretending there is somewhere above This PC. */
static size_t parent_place(void)
{
    const size_t here = current_place();

    if (here == (size_t)-1 || places[here].depth == 0U) {
        return (size_t)-1;
    }
    for (size_t scan = here; scan > 0U; ) {
        --scan;
        if (places[scan].present && places[scan].depth == 0U) {
            return scan;
        }
    }
    return (size_t)-1;
}

/*
 * Which place a breadcrumb segment names, or none.
 *
 * The breadcrumb was five words in a box: the pointer turned into a link
 * over every one of them and clicking did nothing at all.  A segment whose
 * text is a place in the tree can honestly be navigated to, and one that is
 * not - "Users", "phip", folders on a disk this shell has no reader for -
 * is drawn and hit-tested as plain text.  The link is then a promise the
 * window keeps rather than one it makes about everything.
 */
static size_t crumb_place(size_t index)
{
    if (index >= EXPLORER_MAX_CRUMBS || crumbs[index][0] == '\0') {
        return (size_t)-1;
    }
    for (size_t scan = 0U; scan < EXPLORER_MAX_PLACES; ++scan) {
        if (places[scan].present &&
                names_match(places[scan].label, crumbs[index])) {
            return scan;
        }
    }
    return (size_t)-1;
}

/* Defined with the command palette's matching, which wants the same rule:
 * case-insensitive, anywhere in the string, and an empty needle matches
 * everything - which is exactly what an empty search box has to do. */
static bool contains_ci(const char *haystack, const char *needle);

/*
 * Whether a row actually shows: present, and matching the search box if
 * anything is typed into it.
 *
 * Everything below reads the list through this rather than through
 * items[].present, so a filter narrows the rows, the hit targets, the
 * selection and the count in one move instead of four.  Windows matches a
 * name anywhere rather than only at its start - typing "port" finds
 * "reports" - and so does this.
 */
static bool item_visible(size_t index)
{
    if (!items[index].present) {
        return false;
    }
    /* The row being renamed always shows.  Otherwise typing a name the
     * filter does not match takes the row out of the layout mid-edit and
     * leaves the editor on a line that is no longer there. */
    if (rename_active && index == rename_index) {
        return true;
    }
    return contains_ci(items[index].name, search_query);
}

/*
 * The name a row SORTS by, which is the committed one - not what is
 * currently in the editor.
 *
 * Sorting by the live edit means the row climbs the list under the caret,
 * one place per keystroke.  Windows re-sorts when the rename is committed
 * and not before, and so does this: rename_original is the name the row had
 * when the editor opened, and the row goes where the new name belongs the
 * moment Enter says that is its name.
 */
static const char *sort_name(size_t index)
{
    if (rename_active && index == rename_index) {
        return rename_original;
    }
    return items[index].name;
}

/* Defined with the command palette's matching, which wants the same rule. */
static uint32_t lower_ascii(uint32_t code);

/* Case-insensitively: -1, 0 or 1.  "README" and "readme" are neighbours in
 * a file list rather than half the alphabet apart. */
static int compare_ci(const char *left, const char *right)
{
    for (size_t index = 0U; ; ++index) {
        const uint32_t a = lower_ascii((uint32_t)(uint8_t)left[index]);
        const uint32_t b = lower_ascii((uint32_t)(uint8_t)right[index]);

        if (a != b) {
            return a < b ? -1 : 1;
        }
        if (a == 0U) {
            return 0;
        }
    }
}

/*
 * A date the list can be ordered by, out of the string the caller handed it.
 *
 * There is no filesystem behind this window and so no timestamp: the Date
 * modified column is whatever explorer_set_item() was given.  This reads the
 * one shape the shell itself writes - dd/mm/yyyy hh:mm - into a number that
 * sorts, and gives anything it cannot read the largest key there is.  That
 * is less a fallback than the right answer for the only unreadable value
 * this window produces on its own: a row it has just made says "Just now",
 * and just now IS the newest thing in the folder.
 */
static uint64_t date_key(const char *body)
{
    uint32_t field[5] = { 0U, 0U, 0U, 0U, 0U };   /* dd mm yyyy hh mm */
    size_t part = 0U;
    bool inside = false;

    for (size_t at = 0U; body[at] != '\0'; ++at) {
        if (body[at] >= '0' && body[at] <= '9') {
            if (part >= 5U) {
                return UINT64_MAX;   /* more numbers than a date has */
            }
            field[part] = field[part] * 10U + (uint32_t)(body[at] - '0');
            inside = true;
        } else if (inside) {
            ++part;
            inside = false;
        }
    }
    if (inside) {
        ++part;
    }
    if (part != 5U || field[2] < 1000U) {
        return UINT64_MAX;
    }
    return (uint64_t)field[2] * UINT64_C(100000000) +
        (uint64_t)field[1] * UINT64_C(1000000) +
        (uint64_t)field[0] * UINT64_C(10000) +
        (uint64_t)field[3] * UINT64_C(100) + field[4];
}

/*
 * And a size, out of the same kind of display string: "184 KB", "7.4 MB",
 * "0 bytes", or empty for a folder.  One decimal place is read because that
 * is all these strings ever carry; anything past it would be under a byte of
 * a kilobyte and is skipped rather than rounded.
 */
static uint64_t size_key(const char *body)
{
    uint64_t whole = 0U;
    uint64_t tenths = 0U;
    uint64_t unit = 1U;
    size_t at = 0U;

    while (body[at] >= '0' && body[at] <= '9') {
        whole = whole * 10U + (uint64_t)(body[at] - '0');
        ++at;
    }
    if (body[at] == '.') {
        ++at;
        if (body[at] >= '0' && body[at] <= '9') {
            tenths = (uint64_t)(body[at] - '0');
            ++at;
        }
        while (body[at] >= '0' && body[at] <= '9') {
            ++at;
        }
    }
    while (body[at] == ' ') {
        ++at;
    }
    switch (lower_ascii((uint32_t)(uint8_t)body[at])) {
    case 'k':
        unit = UINT64_C(1024);
        break;
    case 'm':
        unit = UINT64_C(1048576);
        break;
    case 'g':
        unit = UINT64_C(1073741824);
        break;
    default:
        break;
    }
    return whole * unit + tenths * unit / 10U;
}

/*
 * Whether A comes before B in the order the list is currently in.
 *
 * Folders first whatever the column: Windows does that in every one of its
 * own sorts, and a list that scatters folders among files is one you have to
 * read rather than scan.  Name is the last tiebreak in every column, and the
 * row's own index is the one after that - which is deliberately NOT reversed
 * by a descending sort, because a comparator that disagrees with itself
 * about two equal rows would leave two of them claiming the same line.
 */
static bool sorts_before(size_t left, size_t right)
{
    const bool folder_left = items[left].kind == EXPLORER_FOLDER;
    const bool folder_right = items[right].kind == EXPLORER_FOLDER;
    int order = 0;

    if (folder_left != folder_right) {
        return folder_left;
    }
    switch (sort_column) {
    case EXPLORER_SORT_MODIFIED: {
        const uint64_t a = date_key(items[left].modified);
        const uint64_t b = date_key(items[right].modified);

        order = a == b ? 0 : (a < b ? -1 : 1);
        break;
    }
    case EXPLORER_SORT_TYPE:
        order = compare_ci(items[left].type, items[right].type);
        break;
    case EXPLORER_SORT_SIZE: {
        const uint64_t a = size_key(items[left].size);
        const uint64_t b = size_key(items[right].size);

        order = a == b ? 0 : (a < b ? -1 : 1);
        break;
    }
    case EXPLORER_SORT_NAME:
    default:
        break;
    }
    if (order == 0) {
        order = compare_ci(sort_name(left), sort_name(right));
    }
    if (order == 0) {
        return left < right;
    }
    return sort_descending ? order > 0 : order < 0;
}

/* Which line a row lands on: how many visible rows sort ahead of it. */
static size_t item_position(size_t index)
{
    size_t position = 0U;

    for (size_t scan = 0U; scan < EXPLORER_MAX_ITEMS; ++scan) {
        if (scan != index && item_visible(scan) &&
                sorts_before(scan, index)) {
            ++position;
        }
    }
    return position;
}

/* How many tiles fit across the list, never fewer than one - a window too
 * narrow for even one tile gets a single column that clips rather than a
 * division by zero. */
static uint32_t tile_columns(void)
{
    const struct ui_rect list = list_rect();
    const uint32_t usable = list.width > EXPLORER_TILE_PAD * 2U ?
        list.width - EXPLORER_TILE_PAD * 2U : 0U;
    const uint32_t fits = usable / EXPLORER_TILE_WIDTH;

    return fits == 0U ? 1U : fits;
}

/*
 * Where a row sits, in whichever view is showing.
 *
 * Both views answer through this one function, so hit-testing, hover,
 * selection and the rename editor follow the view without any of them being
 * taught that there are two.  Switching views is then one bool.
 */
static struct ui_rect item_rect(size_t index)
{
    const struct ui_rect list = list_rect();
    const size_t position = item_position(index);

    if (!item_visible(index)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    if (tiles_view) {
        const uint32_t columns = tile_columns();
        const uint32_t column = (uint32_t)(position % columns);
        const uint32_t row = (uint32_t)(position / columns);
        const uint32_t top = list.y + EXPLORER_TILE_PAD +
            row * EXPLORER_TILE_HEIGHT;

        if (top + EXPLORER_TILE_HEIGHT > list.y + list.height) {
            return (struct ui_rect){ 0U, 0U, 0U, 0U };
        }
        return (struct ui_rect){
            list.x + EXPLORER_TILE_PAD + column * EXPLORER_TILE_WIDTH, top,
            EXPLORER_TILE_WIDTH, EXPLORER_TILE_HEIGHT };
    }
    {
        const uint32_t top = list.y + EXPLORER_HEADER +
            (uint32_t)position * EXPLORER_ROW;

        if (top + EXPLORER_ROW > list.y + list.height) {
            return (struct ui_rect){ 0U, 0U, 0U, 0U };
        }
        return (struct ui_rect){ list.x, top, list.width, EXPLORER_ROW };
    }
}

/*
 * A column heading's own hit target.
 *
 * The headings were drawn with a divider between each pair, which is the
 * one thing that makes a header read as something you can act on - and
 * nothing acted on it.  Clicking one now sorts by it, and clicking the one
 * already sorted by reverses it, which is what the header has looked like
 * it would do since it was first drawn.
 */
static struct ui_rect header_rect(size_t column)
{
    static const uint32_t columns[EXPLORER_SORT_COUNT] = {
        EXPLORER_COLUMN_NAME, EXPLORER_COLUMN_MODIFIED,
        EXPLORER_COLUMN_TYPE, EXPLORER_COLUMN_SIZE };
    const struct ui_rect list = list_rect();
    uint32_t left;
    uint32_t right;

    if (tiles_view || column >= EXPLORER_SORT_COUNT) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    left = list.x + columns[column];
    right = column + 1U < EXPLORER_SORT_COUNT ?
        list.x + columns[column + 1U] : list.x + list.width;
    if (right > list.x + list.width) {
        right = list.x + list.width;
    }
    if (right <= left) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ left, list.y, right - left, EXPLORER_HEADER };
}

struct ui_rect explorer_bounds(void)
{
    return window_rect;
}

/* =========================================================== COMMAND BAR
 *
 * Eight controls in one row, in place of Windows 10's four tabs over five
 * ribbon groups.  See the note beside EXPLORER_COMMANDS for why.
 *
 * The left group acts on the SELECTION and the right group changes how the
 * list is SHOWN, which is the split Windows 11 arrived at and the one that
 * makes a bar this short readable without labels on every button.  Only
 * New, Sort and View carry a word; the five in the middle are the marks
 * everybody already knows, and a bar that spelled all eight out would be
 * two hundred pixels wider for nothing.
 */
enum explorer_command {
    EXPLORER_COMMAND_NEW = 0,
    EXPLORER_COMMAND_CUT,
    EXPLORER_COMMAND_COPY,
    EXPLORER_COMMAND_PASTE,
    EXPLORER_COMMAND_RENAME,
    EXPLORER_COMMAND_DELETE,
    EXPLORER_COMMAND_SORT,
    EXPLORER_COMMAND_VIEW,
    EXPLORER_COMMAND_COUNT
};

struct command_spec {
    const char *label;      /* NULL draws the mark alone */
    const char *glyph;
    bool menu;              /* carries a chevron and opens a flyout */
    bool right;             /* sits in the right-hand group */
    bool separator_after;
};

static const struct command_spec command_specs[EXPLORER_COMMAND_COUNT] = {
    { "New", "folder-plus", true, false, true },
    { NULL, "scissors", false, false, false },
    { NULL, "copy", false, false, false },
    { NULL, "clipboard-paste", false, false, true },
    { NULL, "pencil", false, false, false },
    { NULL, "trash-2", false, false, false },
    { "Sort", "arrow-up", true, true, false },
    { "View", "list", true, true, false }
};

/*
 * Sort and View draw the state they are in rather than a fixed mark, so the
 * bar answers "how is this list arranged" without being opened.  It is the
 * one thing a menu button can do that a menu cannot.
 */
static const char *command_glyph(size_t index)
{
    if (index == EXPLORER_COMMAND_SORT) {
        return sort_descending ? "arrow-down" : "arrow-up";
    }
    if (index == EXPLORER_COMMAND_VIEW) {
        return tiles_view ? "layout-grid" : "list";
    }
    return command_specs[index].glyph;
}

static uint32_t command_width(size_t index)
{
    const struct command_spec *spec = &command_specs[index];
    uint32_t width = EXPLORER_COMMAND_PAD + EXPLORER_COMMAND_ICON +
        EXPLORER_COMMAND_PAD;

    if (spec->label != NULL) {
        width += width_of(spec->label) + 6U;
    }
    if (spec->menu) {
        width += 12U;
    }
    return width;
}

/*
 * Where a control sits.  The left group walks out from the left edge and
 * the right group walks in from the right, so the two never collide on a
 * narrow window - the left group is simply clipped by the bar's own width
 * before it reaches the right one.
 */
static struct ui_rect command_rect(size_t index)
{
    const struct ui_rect bar = commands_rect();
    uint32_t pen;

    if (index >= EXPLORER_COMMAND_COUNT) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    if (!command_specs[index].right) {
        pen = bar.x + EXPLORER_COMMAND_GAP;
        for (size_t scan = 0U; scan < index; ++scan) {
            if (command_specs[scan].right) {
                continue;
            }
            pen += command_width(scan);
            if (command_specs[scan].separator_after) {
                pen += EXPLORER_COMMAND_SEPARATOR;
            }
        }
        return (struct ui_rect){ pen, bar.y + 4U, command_width(index),
            bar.height - 8U };
    }
    pen = bar.x + bar.width - EXPLORER_COMMAND_GAP;
    for (size_t scan = EXPLORER_COMMAND_COUNT; scan > index; ) {
        --scan;
        if (!command_specs[scan].right) {
            continue;
        }
        pen -= command_width(scan);
    }
    return (struct ui_rect){ pen, bar.y + 4U, command_width(index),
        bar.height - 8U };
}

/* The first row the list is showing that is selected, or none.  Selection is
 * one row at a time in this window, so "the first" and "the" are the same
 * thing - but a filtered-away row must not count, or Cut would act on
 * something that is not on screen. */
static size_t selected_item(void)
{
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (item_visible(index) && items[index].selected) {
            return index;
        }
    }
    return (size_t)-1;
}

/* Whether a control can do anything right now.  A disabled control is drawn
 * faint and ignores a click, which is what Windows does rather than acting
 * on nothing. */
static bool command_enabled(size_t index)
{
    switch (index) {
    case EXPLORER_COMMAND_CUT:
    case EXPLORER_COMMAND_COPY:
    case EXPLORER_COMMAND_RENAME:
    case EXPLORER_COMMAND_DELETE:
        return selected_item() != (size_t)-1;
    case EXPLORER_COMMAND_PASTE:
        return clipboard_full;
    default:
        return true;
    }
}

static enum explorer_status draw_commands(struct ui_rect damage)
{
    const struct ui_rect bar = commands_rect();
    enum explorer_status status = fill(bar, damage, chrome);

    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ bar.x, bar.y + bar.height - 1U,
            bar.width, 1U }, damage, rule);
    }
    for (size_t index = 0U; index < EXPLORER_COMMAND_COUNT &&
            status == EXPLORER_STATUS_OK; ++index) {
        const struct command_spec *spec = &command_specs[index];
        const struct ui_rect box = command_rect(index);
        const bool live = command_enabled(index);
        const bool lit = index == open_menu;
        const struct explorer_rgb tint = live ? ink : ink_faint;
        uint32_t pen = box.x + EXPLORER_COMMAND_PAD;

        if (box.width == 0U || box.x + box.width > bar.x + bar.width) {
            continue;
        }
        if (lit) {
            /* An open menu keeps its button pressed, which is the only
             * thing tying the flyout to what opened it. */
            status = fill(box, damage, select_fill);
        } else if (index == hover_command && live) {
            status = fill(box, damage, hover_fill);
        }
        if (status == EXPLORER_STATUS_OK) {
            status = draw_glyph(command_glyph(index), (struct ui_rect){ pen,
                box.y + (box.height - EXPLORER_COMMAND_ICON) / 2U,
                EXPLORER_COMMAND_ICON, EXPLORER_COMMAND_ICON }, damage,
                tint);
        }
        pen += EXPLORER_COMMAND_ICON + 6U;
        if (status == EXPLORER_STATUS_OK && spec->label != NULL) {
            status = text_at(damage, pen, box.y + box.height / 2U + 5U,
                spec->label, tint);
            pen += width_of(spec->label) + 2U;
        }
        if (status == EXPLORER_STATUS_OK && spec->menu) {
            status = draw_glyph("chevron-down", (struct ui_rect){ pen,
                box.y + (box.height - 16U) / 2U, 16U, 16U }, damage, tint);
        }
        if (status == EXPLORER_STATUS_OK && spec->separator_after) {
            status = fill((struct ui_rect){
                box.x + box.width + EXPLORER_COMMAND_SEPARATOR / 2U,
                box.y + 6U, 1U, box.height - 12U }, damage, rule);
        }
    }
    return status;
}

/* ================================================================== MENUS
 *
 * The three flyouts the bar's chevrons open, through one machine.
 *
 * They differ in their rows and in nothing else - the same panel, the same
 * hover, the same "a click that misses dismisses it" rule - so the rows are
 * what varies and the rest is written once.  An earlier version of this
 * file carried the New menu alone as its own open flag, hover index,
 * geometry, drawing, hit test and dismissal; two more menus that way would
 * have been two more copies of all six.
 *
 * Every row is one action or one setting.  There is no submenu, nothing
 * that opens a dialogue, and nothing greyed out: a menu here is a short
 * list of things that happen.
 */

struct new_menu_entry {
    enum explorer_kind kind;
    const char *label;
    const char *glyph;
    const char *default_stem;
    const char *suffix;
    const char *type_label;
};

static const struct new_menu_entry new_menu_entries[] = {
    { EXPLORER_FOLDER, "Folder", "folder-yellow", "New folder", "",
      "File folder" },
    { EXPLORER_TEXT, "Text Document", "file-text", "New Text Document",
      ".txt", "Text Document" },
    { EXPLORER_IMAGE, "Bitmap image", "file-image", "New Bitmap image",
      ".bmp", "Bitmap image" }
};

#define NEW_MENU_COUNT \
    (sizeof(new_menu_entries) / sizeof(new_menu_entries[0]))

/* Sort's rows: the four columns, then the two directions. */
static const char *const sort_labels[EXPLORER_SORT_COUNT] = {
    "Name", "Date modified", "Type", "Size"
};
#define SORT_MENU_COUNT (EXPLORER_SORT_COUNT + 2U)
#define VIEW_MENU_COUNT 2U

#define MENU_ROW 30U
#define MENU_WIDTH 204U
#define MENU_ICON 16U
#define MENU_CHECK 16U
#define MENU_INSET 10U
#define MENU_SEPARATOR 9U

static size_t menu_row_count(size_t command)
{
    switch (command) {
    case EXPLORER_COMMAND_NEW:
        return NEW_MENU_COUNT;
    case EXPLORER_COMMAND_SORT:
        return SORT_MENU_COUNT;
    case EXPLORER_COMMAND_VIEW:
        return VIEW_MENU_COUNT;
    default:
        return 0U;
    }
}

static const char *menu_row_label(size_t command, size_t row)
{
    switch (command) {
    case EXPLORER_COMMAND_NEW:
        return new_menu_entries[row].label;
    case EXPLORER_COMMAND_SORT:
        return row < EXPLORER_SORT_COUNT ? sort_labels[row] :
            (row == EXPLORER_SORT_COUNT ? "Ascending" : "Descending");
    case EXPLORER_COMMAND_VIEW:
        return row == 0U ? "Details" : "Tiles";
    default:
        return "";
    }
}

/* The mark in the row's icon column, or NULL for none.  The CHECK column
 * beside it is separate, so a row never has to choose between saying what
 * it is and saying whether it is on. */
static const char *menu_row_glyph(size_t command, size_t row)
{
    switch (command) {
    case EXPLORER_COMMAND_NEW:
        return new_menu_entries[row].glyph;
    case EXPLORER_COMMAND_SORT:
        if (row == EXPLORER_SORT_COUNT) {
            return "arrow-up";
        }
        return row == EXPLORER_SORT_COUNT + 1U ? "arrow-down" : NULL;
    case EXPLORER_COMMAND_VIEW:
        return row == 0U ? "list" : "layout-grid";
    default:
        return NULL;
    }
}

static bool menu_row_checked(size_t command, size_t row)
{
    switch (command) {
    case EXPLORER_COMMAND_SORT:
        if (row < EXPLORER_SORT_COUNT) {
            return (size_t)sort_column == row;
        }
        return (row == EXPLORER_SORT_COUNT) != sort_descending;
    case EXPLORER_COMMAND_VIEW:
        return (row == 0U) != tiles_view;
    default:
        return false;   /* New makes something; nothing there is "current" */
    }
}

/* A hairline above the row, where a menu changes subject. */
static bool menu_row_separator(size_t command, size_t row)
{
    return command == EXPLORER_COMMAND_SORT && row == EXPLORER_SORT_COUNT;
}

static uint32_t menu_height(size_t command)
{
    const size_t rows = menu_row_count(command);
    uint32_t height = (uint32_t)rows * MENU_ROW + 8U;

    for (size_t row = 0U; row < rows; ++row) {
        if (menu_row_separator(command, row)) {
            height += MENU_SEPARATOR;
        }
    }
    return height;
}

/*
 * The panel, hung under the button that owns it.  A right-group menu is
 * hung by its RIGHT edge instead, because View sits at the window's right
 * margin and a panel two hundred pixels wide dropped from its left edge
 * would be half off the screen.
 */
static struct ui_rect menu_rect(void)
{
    struct ui_rect anchor;
    uint32_t left;

    if (open_menu >= EXPLORER_COMMAND_COUNT) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    anchor = command_rect(open_menu);
    if (anchor.width == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    if (command_specs[open_menu].right) {
        const uint32_t right = anchor.x + anchor.width;

        left = right > MENU_WIDTH ? right - MENU_WIDTH : 0U;
    } else {
        left = anchor.x;
    }
    return (struct ui_rect){ left, anchor.y + anchor.height + 2U, MENU_WIDTH,
        menu_height(open_menu) };
}

static struct ui_rect menu_row_rect(size_t row)
{
    const struct ui_rect menu = menu_rect();
    uint32_t top;

    if (menu.width == 0U || row >= menu_row_count(open_menu)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    top = menu.y + 4U;
    for (size_t scan = 0U; scan < row; ++scan) {
        top += MENU_ROW;
        if (menu_row_separator(open_menu, scan + 1U)) {
            top += MENU_SEPARATOR;
        }
    }
    return (struct ui_rect){ menu.x, top, menu.width, MENU_ROW };
}

static enum explorer_status draw_menu(struct ui_rect damage)
{
    const struct ui_rect menu = menu_rect();
    const size_t rows = menu_row_count(open_menu);
    enum explorer_status status;

    if (menu.width == 0U) {
        return EXPLORER_STATUS_OK;
    }
    status = fill(menu, damage, list_fill);
    if (status == EXPLORER_STATUS_OK) {
        status = outline(menu, damage, rule);
    }
    for (size_t row = 0U; row < rows && status == EXPLORER_STATUS_OK;
         ++row) {
        const struct ui_rect box = menu_row_rect(row);
        const char *glyph = menu_row_glyph(open_menu, row);
        uint32_t pen = box.x + MENU_INSET;

        if (menu_row_separator(open_menu, row)) {
            status = fill((struct ui_rect){ box.x + MENU_INSET,
                box.y - MENU_SEPARATOR / 2U, box.width - MENU_INSET * 2U,
                1U }, damage, rule_soft);
            if (status != EXPLORER_STATUS_OK) {
                return status;
            }
        }
        if (row == menu_hover) {
            status = fill(box, damage, select_fill);
            if (status == EXPLORER_STATUS_OK) {
                status = fill((struct ui_rect){ box.x, box.y,
                    EXPLORER_SELECT_BAR, box.height }, damage, accent);
            }
            if (status != EXPLORER_STATUS_OK) {
                return status;
            }
        }
        if (menu_row_checked(open_menu, row)) {
            status = draw_glyph("check", (struct ui_rect){ pen, box.y,
                MENU_CHECK, box.height }, damage, accent);
            if (status != EXPLORER_STATUS_OK) {
                return status;
            }
        }
        pen += MENU_CHECK + 4U;
        if (glyph != NULL) {
            status = draw_glyph(glyph, (struct ui_rect){ pen, box.y,
                MENU_ICON, box.height }, damage, ink_soft);
            if (status != EXPLORER_STATUS_OK) {
                return status;
            }
        }
        pen += MENU_ICON + 8U;
        status = text_at(damage, pen, box.y + box.height / 2U + 5U,
            menu_row_label(open_menu, row), ink);
    }
    return status;
}

/* ======================================================= COMMAND PALETTE
 *
 * Ctrl+K.  See explorer_toggle_command_palette() in the header for what it
 * is; this is the geometry and the drawing.  The action dispatch that a
 * chosen command actually RUNS lives down in ACTIONS, next to
 * explorer_create_item() and the other things it can call.
 */

#define PALETTE_WIDTH 460U
#define PALETTE_INPUT_HEIGHT 44U
#define PALETTE_ROW 34U
#define PALETTE_ICON 16U
#define PALETTE_INSET 14U
/* More commands than this and the palette would run off the bottom of the
 * window it is floating over; the query keeps narrowing past it instead of
 * the palette growing to fit, which is what every real one does too. */
#define PALETTE_MAX_VISIBLE 8U

static uint32_t lower_ascii(uint32_t code)
{
    return code >= 'A' && code <= 'Z' ? code - 'A' + 'a' : code;
}

/* Whether `needle` occurs in `haystack`, case-insensitively.  An empty
 * needle occurs in anything, which is what an empty query has to do to make
 * every command a match. */
static bool contains_ci(const char *haystack, const char *needle)
{
    if (needle[0] == '\0') {
        return true;
    }
    for (const char *scan = haystack; *scan != '\0'; ++scan) {
        const char *a = scan;
        const char *b = needle;

        while (*a != '\0' && *b != '\0' &&
                lower_ascii((unsigned char)*a) ==
                    lower_ascii((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool starts_with_ci(const char *body, const char *prefix)
{
    while (*prefix != '\0') {
        if (lower_ascii((unsigned char)*body) !=
                lower_ascii((unsigned char)*prefix)) {
            return false;
        }
        ++body;
        ++prefix;
    }
    return true;
}

/*
 * The full command list, in a fixed order: the six things this window can
 * always do, then one "Go to" per place currently in the tree.  Rebuilt
 * every time the palette opens or the query changes rather than kept in
 * step incrementally - cheap at PALETTE_CAPACITY entries, and cheap beats
 * a second place this list can drift from what places[] actually holds.
 */
/*
 * Give the search box the caret, or take it away.
 *
 * The palette and the search box are the same kind of thing - a strip of
 * text this window filters something by - and this shell has exactly one
 * blinking caret to lend out, so focusing either one closes the other
 * rather than leaving two boxes both looking like they are listening.
 * Dropping focus does NOT clear the query: a filter you clicked away from
 * is still a filter, and the rows behind it stay hidden until the x or
 * Escape says otherwise.
 */
static void set_search_focus(bool wanted)
{
    if (wanted) {
        palette_open = false;
        open_menu = (size_t)-1;
        menu_hover = (size_t)-1;
        caret_visible = true;
        caret_phase = (uint32_t)(clock_monotonic_ns() /
            UINT64_C(530000000)) % 2U;
    }
    search_focused = wanted;
}

/* Empties the box and puts every filtered row back. */
static void clear_search(void)
{
    search_query[0] = '\0';
    search_query_length = 0U;
    search_clear_hot = false;
}

static void rebuild_palette_commands(void)
{
    struct palette_command *slot;

    palette_all_count = 0U;
    slot = &palette_all[palette_all_count++];
    copy_field(slot->label, "New folder", sizeof(slot->label));
    slot->glyph = "folder-plus";
    slot->action = PALETTE_NEW_FOLDER;

    slot = &palette_all[palette_all_count++];
    copy_field(slot->label, "New Text Document", sizeof(slot->label));
    slot->glyph = "file-plus";
    slot->action = PALETTE_NEW_TEXT;

    slot = &palette_all[palette_all_count++];
    copy_field(slot->label, "New Bitmap image", sizeof(slot->label));
    slot->glyph = "file-plus";
    slot->action = PALETTE_NEW_BITMAP;

    slot = &palette_all[palette_all_count++];
    copy_field(slot->label, "Select all", sizeof(slot->label));
    slot->glyph = "square-check";
    slot->action = PALETTE_SELECT_ALL;

    slot = &palette_all[palette_all_count++];
    copy_field(slot->label, "Select none", sizeof(slot->label));
    slot->glyph = "square";
    slot->action = PALETTE_SELECT_NONE;

    slot = &palette_all[palette_all_count++];
    copy_field(slot->label, "Invert selection", sizeof(slot->label));
    slot->glyph = "rotate-ccw";
    slot->action = PALETTE_INVERT_SELECTION;

    slot = &palette_all[palette_all_count++];
    /* Its own label rather than a fixed one, since what the command is
     * about to DO depends on which view is up - "Tiles" from the table,
     * "Details" from the tiles. */
    copy_field(slot->label, tiles_view ? "Details view" : "Tiles view",
        sizeof(slot->label));
    slot->glyph = tiles_view ? "list" : "layout-grid";
    slot->action = PALETTE_TOGGLE_VIEW;

    /*
     * Only while the search box has something in it.  A palette that always
     * offers "Clear search" is a palette telling you about a filter you may
     * not have; one that offers it exactly when rows are hidden is telling
     * you why the list looks short.
     */
    if (search_query[0] != '\0') {
        slot = &palette_all[palette_all_count++];
        copy_field(slot->label, "Clear search", sizeof(slot->label));
        slot->glyph = "x";
        slot->action = PALETTE_CLEAR_SEARCH;
    }

    for (size_t index = 0U; index < EXPLORER_MAX_PLACES &&
            palette_all_count < PALETTE_CAPACITY; ++index) {
        size_t at;

        if (!places[index].present) {
            continue;
        }
        slot = &palette_all[palette_all_count++];
        at = append_literal(slot->label, sizeof(slot->label), 0U, "Go to ");
        (void)append_literal(slot->label, sizeof(slot->label), at,
            places[index].label);
        slot->glyph = places[index].glyph == NULL ? "folder" :
            places[index].glyph;
        slot->action = PALETTE_GO_TO_PLACE;
        slot->place_index = index;
    }
}

/* Filters palette_all against the current query into palette_matches, a
 * prefix match sorting ahead of a plain substring one so the entry Enter
 * would run - index [0] - is the one typing the query was most likely
 * reaching for. */
static void rebuild_palette_matches(void)
{
    palette_match_count = 0U;
    for (size_t index = 0U; index < palette_all_count &&
            palette_match_count < PALETTE_CAPACITY; ++index) {
        if (starts_with_ci(palette_all[index].label, palette_query)) {
            palette_matches[palette_match_count++] = index;
        }
    }
    for (size_t index = 0U; index < palette_all_count &&
            palette_match_count < PALETTE_CAPACITY; ++index) {
        if (starts_with_ci(palette_all[index].label, palette_query)) {
            continue;
        }
        if (contains_ci(palette_all[index].label, palette_query)) {
            palette_matches[palette_match_count++] = index;
        }
    }
    palette_hover = (size_t)-1;
}

static struct ui_rect palette_rect(void)
{
    const uint32_t visible = palette_match_count < PALETTE_MAX_VISIBLE ?
        (uint32_t)palette_match_count : PALETTE_MAX_VISIBLE;
    /* Room for the row list, or - when a query matches nothing - room for
     * saying so, rather than a message drawn under a box too short to hold
     * it. */
    const uint32_t body = visible > 0U ? visible * PALETTE_ROW + 8U :
        (palette_query[0] != '\0' ? 40U : 0U);

    return (struct ui_rect){
        window_rect.x + (window_rect.width - PALETTE_WIDTH) / 2U,
        window_rect.y + 90U, PALETTE_WIDTH, PALETTE_INPUT_HEIGHT + body };
}

static struct ui_rect palette_input_rect(void)
{
    const struct ui_rect palette = palette_rect();

    return (struct ui_rect){ palette.x, palette.y, palette.width,
        PALETTE_INPUT_HEIGHT };
}

/* `visible` is a position in the ON-SCREEN list, 0 at the top - not an
 * index into palette_matches, so a caller walking rows to draw or hit-test
 * them never has to know palette_matches exists. */
static struct ui_rect palette_row_rect(size_t visible)
{
    const struct ui_rect palette = palette_rect();

    if (visible >= PALETTE_MAX_VISIBLE || visible >= palette_match_count) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ palette.x,
        palette.y + PALETTE_INPUT_HEIGHT + 4U +
            (uint32_t)visible * PALETTE_ROW, palette.width, PALETTE_ROW };
}

static enum explorer_status draw_command_palette(struct ui_rect damage)
{
    const struct ui_rect palette = palette_rect();
    const struct ui_rect input = palette_input_rect();
    enum explorer_status status;

    if (!palette_open) {
        return EXPLORER_STATUS_OK;
    }
    status = fill(palette, damage, list_fill);
    if (status == EXPLORER_STATUS_OK) {
        status = outline(palette, damage, accent);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = draw_glyph("search", (struct ui_rect){
            input.x + PALETTE_INSET, input.y, PALETTE_ICON, input.height },
            damage, ink_soft);
    }
    if (status == EXPLORER_STATUS_OK) {
        const uint32_t text_x = input.x + PALETTE_INSET + PALETTE_ICON + 10U;

        status = text_at(damage, text_x, input.y + input.height / 2U + 5U,
            palette_query[0] != '\0' ? palette_query :
                "Type a command", palette_query[0] != '\0' ? ink :
                ink_faint);
        if (status == EXPLORER_STATUS_OK && caret_visible) {
            status = fill((struct ui_rect){
                text_x + width_of(palette_query) + 1U, input.y + 10U, 1U,
                input.height - 20U }, damage, ink);
        }
    }
    if (status == EXPLORER_STATUS_OK &&
            (palette_match_count > 0U || palette_query[0] != '\0')) {
        status = fill((struct ui_rect){ palette.x, input.y + input.height,
            palette.width, 1U }, damage, rule);
    }
    for (size_t visible = 0U; visible < palette_match_count &&
            visible < PALETTE_MAX_VISIBLE && status == EXPLORER_STATUS_OK;
         ++visible) {
        const struct palette_command *command =
            &palette_all[palette_matches[visible]];
        const struct ui_rect row = palette_row_rect(visible);
        uint32_t pen = row.x + PALETTE_INSET;

        if (visible == palette_hover) {
            status = fill(row, damage, select_fill);
            if (status == EXPLORER_STATUS_OK) {
                status = fill((struct ui_rect){ row.x, row.y,
                    EXPLORER_SELECT_BAR, row.height }, damage, accent);
            }
            if (status != EXPLORER_STATUS_OK) {
                return status;
            }
        }
        status = draw_glyph(command->glyph, (struct ui_rect){ pen, row.y,
            PALETTE_ICON, row.height }, damage,
            command->action == PALETTE_GO_TO_PLACE ?
                kind_colour[EXPLORER_TEXT] : ink_soft);
        if (status != EXPLORER_STATUS_OK) {
            return status;
        }
        pen += PALETTE_ICON + 10U;
        status = text_at(damage, pen, row.y + row.height / 2U + 5U,
            command->label, visible == 0U ? ink : ink_soft);
    }
    if (status == EXPLORER_STATUS_OK && palette_match_count == 0U &&
            palette_query[0] != '\0') {
        status = text_at(damage, input.x + PALETTE_INSET,
            input.y + input.height + 22U, "No matching commands",
            ink_faint);
    }
    return status;
}

/* ================================================================== CHROME */

static enum explorer_status draw_caption(struct ui_rect damage)
{
    const struct ui_rect caption = caption_rect();
    enum explorer_status status = fill(caption, damage, caption_fill);

    /*
     * The window's own mark, and nothing else.
     *
     * Windows 10 tucks a Quick Access Toolbar into the left end of this bar
     * - properties, new folder, and a chevron to customise the set.  A copy
     * of it here drew three buttons of which none did anything, and the
     * one that could have (new folder) is forty pixels below in the command
     * bar already.  Three dead marks at the top left of the window is
     * exactly the kind of thing this window is being cleared of.
     */
    if (status == EXPLORER_STATUS_OK) {
        status = draw_glyph("folder-yellow", (struct ui_rect){
            caption.x + EXPLORER_QAT_INSET, caption.y, EXPLORER_QAT_SLOT,
            caption.height }, damage, kind_colour[EXPLORER_FOLDER]);
    }
    if (status != EXPLORER_STATUS_OK) {
        return status;
    }
    /* Windows centres the caption text over the whole bar, not over what is
     * left of it, so the mark does not shove the title sideways. */
    status = text_at(damage,
        centred_x(caption.x, caption.width, width_of(window_title)),
        caption.y + caption.height / 2U + 5U, window_title,
        focused ? ink : ink_faint);
    for (uint32_t index = 0U; index < 3U && status == EXPLORER_STATUS_OK;
         ++index) {
        const struct ui_rect button = caption_button_rect(index);
        const uint32_t size = 10U;
        const uint32_t left = button.x + (button.width - size) / 2U;
        const uint32_t top = button.y + (button.height - size) / 2U;
        const struct explorer_rgb mark = focused ? ink : ink_faint;

        if (index == 0U) {
            status = fill((struct ui_rect){ left, top + size / 2U, size, 1U },
                damage, mark);
        } else if (index == 1U) {
            status = fill((struct ui_rect){ left, top, size, 1U }, damage,
                mark);
            if (status == EXPLORER_STATUS_OK) {
                status = fill((struct ui_rect){ left, top + size - 1U, size,
                    1U }, damage, mark);
            }
            if (status == EXPLORER_STATUS_OK) {
                status = fill((struct ui_rect){ left, top, 1U, size }, damage,
                    mark);
            }
            if (status == EXPLORER_STATUS_OK) {
                status = fill((struct ui_rect){ left + size - 1U, top, 1U,
                    size }, damage, mark);
            }
        } else {
            for (uint32_t step = 0U; step < size &&
                    status == EXPLORER_STATUS_OK; ++step) {
                status = fill((struct ui_rect){ left + step, top + step, 1U,
                    1U }, damage, mark);
                if (status == EXPLORER_STATUS_OK) {
                    status = fill((struct ui_rect){ left + step,
                        top + size - 1U - step, 1U, 1U }, damage, mark);
                }
            }
        }
    }
    return status;
}

/* Whether each of the three arrows has anywhere to go.  All three were
 * drawn and none of them went anywhere; now they walk the navigation
 * pane's own history, and one with nothing behind it says so by going
 * faint rather than by doing nothing when it is pressed. */
static bool address_slot_enabled(size_t index)
{
    if (index == 0U) {
        return can_go_back();
    }
    if (index == 1U) {
        return can_go_forward();
    }
    return parent_place() != (size_t)-1;
}

static enum explorer_status draw_address(struct ui_rect damage)
{
    static const char *const arrows[] = { "arrow-left", "arrow-right",
        "arrow-up" };
    const struct ui_rect band = address_rect();
    enum explorer_status status = fill(band, damage, chrome);

    for (size_t index = 0U; index < 3U && status == EXPLORER_STATUS_OK;
         ++index) {
        const struct ui_rect slot = address_slot_rect(index);
        const bool live = address_slot_enabled(index);

        if (live && index == hover_address) {
            status = fill((struct ui_rect){ slot.x, slot.y + 3U, slot.width,
                slot.height - 6U }, damage, hover_fill);
        }
        if (status == EXPLORER_STATUS_OK) {
            status = draw_glyph(arrows[index], slot, damage,
                live ? ink_soft : ink_faint);
        }
    }
    if (status != EXPLORER_STATUS_OK) {
        return status;
    }

    const struct ui_rect field = address_field_rect();
    const struct ui_rect search = search_rect();

    status = fill(field, damage, field_fill);
    if (status == EXPLORER_STATUS_OK) {
        status = outline(field, damage, rule);
    }
    if (status != EXPLORER_STATUS_OK) {
        return status;
    }

    /*
     * The breadcrumb.  Windows draws every segment in the same ink and
     * leaves you to work out which one you are standing in; here the LAST
     * one takes the accent, which is the whole of the question the address
     * bar is there to answer.  A segment that names a place in the tree is
     * underlined under the pointer, because that is the one that can be
     * clicked - see crumb_place().
     */
    size_t last = 0U;

    for (size_t index = 0U; index < EXPLORER_MAX_CRUMBS; ++index) {
        if (crumbs[index][0] != '\0') {
            last = index;
        }
    }
    for (size_t index = 0U; index < EXPLORER_MAX_CRUMBS &&
            status == EXPLORER_STATUS_OK; ++index) {
        const struct ui_rect box = crumb_rect(index);

        if (box.width == 0U) {
            continue;
        }
        status = text_at(damage, box.x, box.y + box.height / 2U + 5U,
            crumbs[index], index == last ? accent : ink);
        if (status == EXPLORER_STATUS_OK && index == hover_crumb) {
            status = fill((struct ui_rect){ box.x,
                box.y + box.height / 2U + 7U, box.width, 1U }, damage,
                index == last ? accent : ink);
        }
        if (index != last && status == EXPLORER_STATUS_OK) {
            status = draw_glyph("chevron-right", (struct ui_rect){
                box.x + box.width + 4U, box.y, 16U, box.height }, damage,
                ink_faint);
        }
    }
    if (status != EXPLORER_STATUS_OK) {
        return status;
    }

    /*
     * The search box.  Focused, it takes the accent on all four sides -
     * the same ring the Windows box grows, and the only thing on this bar
     * that tells you where a keystroke is about to land.
     */
    const struct explorer_rgb edge = search_focused ? accent : rule;
    const uint32_t edge_weight = search_focused ? 2U : 1U;

    status = fill(search, damage, field_fill);
    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ search.x, search.y, search.width,
            edge_weight }, damage, edge);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ search.x,
            search.y + search.height - edge_weight, search.width,
            edge_weight }, damage, edge);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ search.x, search.y, edge_weight,
            search.height }, damage, edge);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){
            search.x + search.width - edge_weight, search.y, edge_weight,
            search.height }, damage, edge);
    }
    if (status != EXPLORER_STATUS_OK) {
        return status;
    }
    /*
     * The magnifier becomes an x once there is something to clear, which is
     * what Windows' own box does rather than carrying both marks or leaving
     * the magnifier there to be clicked to no effect.
     */
    if (search_query[0] != '\0') {
        status = draw_glyph("x", search_clear_rect(), damage,
            search_clear_hot ? accent : ink_soft);
    } else {
        status = draw_glyph("search", (struct ui_rect){
            search.x + search.width - 24U, search.y, 20U, search.height },
            damage, ink_soft);
    }
    if (status != EXPLORER_STATUS_OK) {
        return status;
    }
    status = text_at(damage, search.x + 8U,
        search.y + search.height / 2U + 5U,
        search_query[0] != '\0' ? search_query : "Search Documents",
        search_query[0] != '\0' ? ink : ink_faint);
    /* The caret sits after the text, and only blinks while the box holds
     * focus - there is no caret repositioning here for the same reason the
     * rename editor has none: this platform delivers characters and
     * Backspace to a window and nothing else. */
    if (status == EXPLORER_STATUS_OK && search_focused && caret_visible) {
        status = fill((struct ui_rect){
            search.x + 8U + width_of(search_query) + 1U, search.y + 5U, 1U,
            search.height - 10U }, damage, ink);
    }
    return status;
}

/* ==================================================================== BODY */

static enum explorer_status draw_nav(struct ui_rect damage)
{
    const struct ui_rect nav = nav_rect();
    enum explorer_status status = fill(nav, damage, chrome);

    if (status == EXPLORER_STATUS_OK) {
        /* The hairline between the tree and the list, which is the only
         * thing separating two panes of nearly the same brightness. */
        status = fill((struct ui_rect){ nav.x + nav.width - 1U, nav.y, 1U,
            nav.height }, damage, rule);
    }
    for (size_t index = 0U; index < EXPLORER_MAX_PLACES &&
            status == EXPLORER_STATUS_OK; ++index) {
        const struct explorer_place *place = &places[index];
        const struct ui_rect row = place_rect(index);
        const uint32_t indent = EXPLORER_PAD +
            (uint32_t)place->depth * EXPLORER_TREE_INDENT;
        uint32_t pen;

        if (!place->present || row.width == 0U) {
            continue;
        }
        if (place->current) {
            status = fill(row, damage, select_fill);
            if (status == EXPLORER_STATUS_OK) {
                status = fill((struct ui_rect){ row.x, row.y,
                    EXPLORER_SELECT_BAR, row.height }, damage, accent);
            }
        } else {
            status = blend(row, damage, hover_fill,
                index == hover_place ? ui_motion_alpha(&place_fade) :
                    (index == leaving_place ?
                        ui_motion_alpha(&place_leave_fade) : 0U));
        }
        if (status != EXPLORER_STATUS_OK) {
            return status;
        }
        pen = row.x + indent;
        if (place->expandable) {
            status = draw_glyph(place->expanded ? "chevron-down" :
                "chevron-right", (struct ui_rect){ pen, row.y,
                EXPLORER_TREE_MARK, row.height }, damage, ink_soft);
            if (status != EXPLORER_STATUS_OK) {
                return status;
            }
        }
        pen += EXPLORER_TREE_MARK;
        status = draw_glyph(place->glyph == NULL ? "folder-orange" :
            place->glyph, (struct ui_rect){ pen, row.y, EXPLORER_TREE_ICON,
            row.height }, damage, ink_soft);
        if (status != EXPLORER_STATUS_OK) {
            return status;
        }
        pen += EXPLORER_TREE_ICON + 6U;
        status = text_at(damage, pen, row.y + row.height / 2U + 5U,
            place->label, place->current ? ink : ink_soft);
        if (status == EXPLORER_STATUS_OK && place->pinned) {
            /* Quick access marks its pinned entries; Windows puts the pin at
             * the right-hand end of the row. */
            status = draw_glyph("pin", (struct ui_rect){
                row.x + row.width - 26U, row.y, 16U, row.height }, damage,
                ink_faint);
        }
    }
    return status;
}

/* A row that has been Cut is still in the list and on its way out of it,
 * which Windows says by drawing it at about half strength. */
static bool row_is_cut(size_t index)
{
    return clipboard_full && clipboard_cut && clipboard_source == index;
}

/*
 * The name, plain text or - for the one row mid-rename - an editable box:
 * an accent outline, the stem on an accent plate while it is still the
 * initial selection, the fixed suffix after it untouched, and a blinking
 * caret once the first keystroke has narrowed the selection to nothing.
 *
 * AREA is the box the name is allowed: a column in the details view, the
 * strip under the icon in tiles.  Both views draw a rename through here, so
 * switching views mid-rename moves the editor rather than losing it.
 *
 * There is no caret repositioning: this platform does not yet deliver arrow
 * keys to a window, only characters and Backspace, so the caret is always
 * at the end of what has been typed - which is also where Windows leaves it
 * after typing over a selection, so the common case is exact.
 */
static enum explorer_status draw_name_cell(size_t index,
    struct ui_rect area, uint32_t baseline, struct ui_rect damage,
    struct explorer_rgb colour)
{
    uint32_t stem_w;
    uint32_t suffix_w;
    uint32_t box_width;
    struct ui_rect box;
    enum explorer_status status;

    if (!rename_active || index != rename_index) {
        return text_elided(damage, area.x, baseline, items[index].name,
            colour, area.width);
    }
    stem_w = width_of(rename_stem);
    suffix_w = width_of(rename_suffix);
    box_width = stem_w + suffix_w + 12U;
    if (box_width > area.width) {
        box_width = area.width;
    }
    box = (struct ui_rect){ area.x - 4U, area.y, box_width, area.height };
    status = fill(box, damage, field_fill);
    if (status == EXPLORER_STATUS_OK) {
        status = outline(box, damage, accent);
    }
    if (status == EXPLORER_STATUS_OK && rename_selected &&
            rename_length > 0U) {
        status = fill((struct ui_rect){ area.x - 1U, box.y + 1U, stem_w + 2U,
            box.height - 2U }, damage, accent);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = text_at(damage, area.x, baseline, rename_stem,
            rename_selected && rename_length > 0U ? field_fill : ink);
    }
    if (status == EXPLORER_STATUS_OK && rename_suffix[0] != '\0') {
        status = text_at(damage, area.x + stem_w, baseline, rename_suffix,
            ink);
    }
    if (status == EXPLORER_STATUS_OK && !rename_selected && caret_visible) {
        status = fill((struct ui_rect){ area.x + stem_w + 1U, box.y + 3U, 1U,
            box.height - 6U }, damage, ink);
    }
    return status;
}

/* The plate behind a row, in either view: selected, hovered, or neither. */
static enum explorer_status draw_row_plate(size_t index, struct ui_rect row,
    struct ui_rect damage)
{
    if (items[index].selected) {
        const enum explorer_status status = fill(row, damage, select_fill);

        if (status != EXPLORER_STATUS_OK) {
            return status;
        }
        /* Phipia's accent bar, where Windows lays a flat blue plate over
         * the whole row. */
        return fill((struct ui_rect){ row.x, row.y, EXPLORER_SELECT_BAR,
            row.height }, damage, accent);
    }
    return blend(row, damage, hover_fill,
        index == hover_item ? ui_motion_alpha(&item_fade) :
            (index == leaving_item ? ui_motion_alpha(&item_leave_fade) :
                0U));
}

static const char *const list_headings[EXPLORER_SORT_COUNT] = {
    "Name", "Date modified", "Type", "Size"
};
static const uint32_t list_columns[EXPLORER_SORT_COUNT] = {
    EXPLORER_COLUMN_NAME, EXPLORER_COLUMN_MODIFIED, EXPLORER_COLUMN_TYPE,
    EXPLORER_COLUMN_SIZE
};

/*
 * The column header, which now does what it has always looked like it does.
 *
 * It was four words with a divider between each pair - the exact drawing of
 * a sortable header - over a list that could not be sorted at all.  The
 * heading the list is ordered by carries the chevron, and the chevron
 * points the way the order runs.
 */
static enum explorer_status draw_header(struct ui_rect damage)
{
    const struct ui_rect list = list_rect();
    enum explorer_status status = fill((struct ui_rect){ list.x, list.y,
        list.width, EXPLORER_HEADER }, damage, list_fill);

    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ list.x,
            list.y + EXPLORER_HEADER - 1U, list.width, 1U }, damage, rule);
    }
    for (size_t index = 0U; index < EXPLORER_SORT_COUNT &&
            status == EXPLORER_STATUS_OK; ++index) {
        const struct ui_rect box = header_rect(index);
        const bool sorted = (size_t)sort_column == index;
        const uint32_t x = list.x + EXPLORER_PAD + list_columns[index];

        if (box.width == 0U || x + 40U > list.x + list.width) {
            break;
        }
        status = text_at(damage, x, list.y + EXPLORER_HEADER / 2U + 5U,
            list_headings[index], sorted ? accent : ink_soft);
        if (status == EXPLORER_STATUS_OK && sorted) {
            status = draw_glyph(sort_descending ? "chevron-down" :
                "chevron-up", (struct ui_rect){
                x + width_of(list_headings[index]) + 4U,
                list.y + (EXPLORER_HEADER - EXPLORER_SORT_MARK) / 2U,
                EXPLORER_SORT_MARK, EXPLORER_SORT_MARK }, damage, accent);
        }
        if (status == EXPLORER_STATUS_OK && index != 0U) {
            /* The one-pixel divider between two columns, which is what makes
             * the header read as draggable. */
            status = fill((struct ui_rect){ box.x, list.y + 5U, 1U,
                EXPLORER_HEADER - 10U }, damage, rule_soft);
        }
    }
    return status;
}

static enum explorer_status draw_details(struct ui_rect damage)
{
    enum explorer_status status = draw_header(damage);

    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS &&
            status == EXPLORER_STATUS_OK; ++index) {
        const struct explorer_item *item = &items[index];
        const struct ui_rect row = item_rect(index);
        const uint32_t ghost = row_is_cut(index) ? 128U : 255U;
        const struct explorer_rgb name_ink = row_is_cut(index) ? ink_faint :
            ink;
        const char *fields[3];
        uint32_t pen;

        if (row.width == 0U) {
            continue;
        }
        status = draw_row_plate(index, row, damage);
        if (status != EXPLORER_STATUS_OK) {
            return status;
        }
        pen = row.x + EXPLORER_PAD;
        status = draw_glyph_ghosted(kind_glyph[item->kind],
            (struct ui_rect){ pen, row.y, EXPLORER_ROW_ICON, row.height },
            damage, kind_colour[item->kind], ghost);
        if (status != EXPLORER_STATUS_OK) {
            return status;
        }
        pen += EXPLORER_ROW_ICON + 8U;
        status = draw_name_cell(index, (struct ui_rect){ pen, row.y + 2U,
            row.x + EXPLORER_PAD + EXPLORER_COLUMN_MODIFIED - pen -
                EXPLORER_PAD, row.height - 4U },
            row.y + row.height / 2U + 5U, damage, name_ink);
        fields[0] = item->modified;
        fields[1] = item->type;
        fields[2] = item->size;
        for (size_t field = 0U; field < 3U &&
                status == EXPLORER_STATUS_OK; ++field) {
            const uint32_t x = row.x + EXPLORER_PAD +
                list_columns[field + 1U];
            const uint32_t limit = field + 2U < EXPLORER_SORT_COUNT ?
                list_columns[field + 2U] - list_columns[field + 1U] -
                    EXPLORER_PAD :
                row.x + row.width - x;

            if (x + 40U > row.x + row.width || fields[field][0] == '\0') {
                continue;
            }
            status = text_elided(damage, x, row.y + row.height / 2U + 5U,
                fields[field], ink_soft, limit);
        }
    }
    return status;
}

/*
 * TILES: the same rows as big icons with the name under each.
 *
 * Windows 10's own tile view puts the type and the size beside the name in
 * two more lines of grey; at 48 pixels those lines are most of the tile and
 * are the columns you switched away from.  This keeps the name alone, which
 * is what the view is for - looking at pictures and folders rather than
 * reading a table.
 */
static enum explorer_status draw_tiles(struct ui_rect damage)
{
    enum explorer_status status = EXPLORER_STATUS_OK;

    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS &&
            status == EXPLORER_STATUS_OK; ++index) {
        const struct explorer_item *item = &items[index];
        const struct ui_rect tile = item_rect(index);
        const uint32_t ghost = row_is_cut(index) ? 128U : 255U;
        const struct explorer_rgb name_ink = row_is_cut(index) ? ink_faint :
            ink;
        struct ui_rect strip;
        uint32_t name_width;

        if (tile.width == 0U) {
            continue;
        }
        status = draw_row_plate(index, tile, damage);
        if (status != EXPLORER_STATUS_OK) {
            return status;
        }
        status = draw_glyph_ghosted(kind_glyph[item->kind],
            (struct ui_rect){
                tile.x + (tile.width - EXPLORER_TILE_ICON) / 2U,
                tile.y + 10U, EXPLORER_TILE_ICON, EXPLORER_TILE_ICON },
            damage, kind_colour[item->kind], ghost);
        if (status != EXPLORER_STATUS_OK) {
            return status;
        }
        /* The name is centred while it is a label and left-aligned while it
         * is an editor, because a caret that moves the text it is at the
         * end of is a caret you cannot type against. */
        name_width = width_of(item->name);
        strip = (struct ui_rect){ tile.x + 8U,
            tile.y + EXPLORER_TILE_ICON + 16U, tile.width - 16U, 20U };
        if (!rename_active || index != rename_index) {
            const uint32_t shown = name_width < strip.width ? name_width :
                strip.width;

            strip.x = centred_x(strip.x, strip.width, shown);
            strip.width = shown;
        }
        status = draw_name_cell(index, strip, strip.y + 14U, damage,
            name_ink);
    }
    return status;
}

static enum explorer_status draw_list(struct ui_rect damage)
{
    const struct ui_rect list = list_rect();
    enum explorer_status status = fill(list, damage, list_fill);

    if (status != EXPLORER_STATUS_OK) {
        return status;
    }
    return tiles_view ? draw_tiles(damage) : draw_details(damage);
}

/* The two view toggles at the far right of the status bar - details on the
 * left, tiles on the right.  Windows puts them exactly there, and this is
 * the second way to reach the same setting the View menu holds. */
static struct ui_rect view_toggle_rect(size_t which)
{
    const struct ui_rect bar = status_rect();

    if (which >= 2U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ bar.x + bar.width - (2U - which) * 26U, bar.y,
        24U, bar.height };
}

static enum explorer_status draw_status(struct ui_rect damage)
{
    const struct ui_rect bar = status_rect();
    uint32_t count = 0U;
    uint32_t held = 0U;
    uint32_t chosen = 0U;
    char summary[48];
    size_t at = 0U;
    enum explorer_status status = fill(bar, damage, chrome);

    if (status == EXPLORER_STATUS_OK) {
        status = fill((struct ui_rect){ bar.x, bar.y, bar.width, 1U }, damage,
            rule);
    }
    if (status != EXPLORER_STATUS_OK) {
        return status;
    }
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (!items[index].present) {
            continue;
        }
        ++held;
        if (!item_visible(index)) {
            continue;
        }
        ++count;
        if (items[index].selected) {
            ++chosen;
        }
    }
    /*
     * "3 items", or "2 of 3 items" once the search box is filtering - the
     * count has to say what it is counting, or a filtered window looks like
     * a folder that lost files.  Windows says "N items" over its own search
     * results and leaves the total out; this keeps it, because a filter
     * that hides four of five rows is worth being told about.
     */
    at = append_uint(summary, sizeof(summary), at, count);
    if (count != held) {
        at = append_literal(summary, sizeof(summary), at, " of ");
        at = append_uint(summary, sizeof(summary), at, held);
    }
    at = append_literal(summary, sizeof(summary), at, " item");
    if (count != 1U) {
        (void)append_literal(summary, sizeof(summary), at, "s");
    }
    status = text_at(damage, bar.x + EXPLORER_PAD,
        bar.y + bar.height / 2U + 5U, summary, ink_soft);
    if (status == EXPLORER_STATUS_OK && chosen != 0U) {
        char chose[32];
        size_t where = append_uint(chose, sizeof(chose), 0U, chosen);

        (void)append_literal(chose, sizeof(chose), where, " selected");
        status = text_at(damage,
            bar.x + EXPLORER_PAD + width_of(summary) + 24U,
            bar.y + bar.height / 2U + 5U, chose, ink_faint);
    }
    /* And a word for what the clipboard is holding, which is the only place
     * in the window a Cut or a Copy can be seen from once the button that
     * made it has gone back to looking like a button. */
    if (status == EXPLORER_STATUS_OK && clipboard_full) {
        char note[EXPLORER_NAME_BYTES + 16U];
        size_t where = append_literal(note, sizeof(note), 0U,
            clipboard_cut ? "Cut: " : "Copied: ");

        (void)append_literal(note, sizeof(note), where, clipboard_item.name);
        status = text_elided(damage,
            bar.x + bar.width - 240U, bar.y + bar.height / 2U + 5U, note,
            ink_faint, 180U);
    }
    for (size_t which = 0U; which < 2U && status == EXPLORER_STATUS_OK;
         ++which) {
        const bool active = (which == 1U) == tiles_view;

        status = draw_glyph(which == 0U ? "list" : "layout-grid",
            view_toggle_rect(which), damage, active ? accent : ink_soft);
    }
    return status;
}

/* ================================================================ ACTIONS */

/*
 * A name for a freshly created item that nothing already present is using.
 *
 * Tries the entry's own default first, then "default (2)", "(3)" and so on,
 * checking the FULL name - stem plus suffix - against every present item,
 * which is what makes this collide correctly against a Text Document named
 * "New Text Document (2).txt" typed by hand.  Windows does the same rather
 * than refusing the plain name outright.
 */
static void unique_stem(const char *base, const char *suffix,
    char *out_stem, size_t out_bytes)
{
    for (uint32_t number = 1U; number <= EXPLORER_MAX_ITEMS + 1U; ++number) {
        char candidate[EXPLORER_NAME_BYTES];
        char full[EXPLORER_NAME_BYTES];
        size_t stem_len = append_literal(candidate, sizeof(candidate), 0U,
            base);
        size_t full_len;
        bool collides = false;

        if (number > 1U) {
            stem_len = append_literal(candidate, sizeof(candidate),
                stem_len, " (");
            stem_len = append_uint(candidate, sizeof(candidate), stem_len,
                number);
            stem_len = append_literal(candidate, sizeof(candidate),
                stem_len, ")");
        }
        full_len = append_literal(full, sizeof(full), 0U, candidate);
        (void)append_literal(full, sizeof(full), full_len, suffix);
        for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
            if (items[index].present &&
                    names_match(items[index].name, full)) {
                collides = true;
                break;
            }
        }
        if (!collides) {
            copy_field(out_stem, candidate, out_bytes);
            return;
        }
    }
    /* Past EXPLORER_MAX_ITEMS collisions the list is full of nothing but
     * this name's own numbering; Windows would run out of room too. */
    copy_field(out_stem, base, out_bytes);
}

/*
 * A name split where the extension starts - the LAST dot, and never the
 * first character, so ".gitignore" is a name with no extension rather than
 * an extension with no name.  Windows draws that distinction the same way,
 * and it is the one that decides what a rename highlights and what " -
 * Copy" is inserted in front of.
 */
static void split_name(const char *name, char *stem, size_t stem_bytes,
    char *suffix, size_t suffix_bytes)
{
    size_t length = 0U;
    size_t dot = 0U;
    bool found = false;

    while (name[length] != '\0') {
        if (name[length] == '.' && length > 0U) {
            dot = length;
            found = true;
        }
        ++length;
    }
    if (!found) {
        copy_field(stem, name, stem_bytes);
        copy_field(suffix, "", suffix_bytes);
        return;
    }
    {
        size_t at = 0U;

        while (at < dot && at + 1U < stem_bytes) {
            stem[at] = name[at];
            ++at;
        }
        stem[at] = '\0';
    }
    copy_field(suffix, &name[dot], suffix_bytes);
}

/* Writes the row's displayed name from the live edit, so a row mid-rename is
 * never showing anything other than what is actually being typed. */
static void sync_rename_name(void)
{
    size_t at;

    if (rename_index >= EXPLORER_MAX_ITEMS) {
        return;
    }
    at = append_literal(items[rename_index].name,
        sizeof(items[rename_index].name), 0U, rename_stem);
    (void)append_literal(items[rename_index].name,
        sizeof(items[rename_index].name), at, rename_suffix);
}

enum explorer_status explorer_create_item(enum explorer_kind kind,
    struct ui_rect *damage)
{
    const struct new_menu_entry *entry = NULL;
    struct explorer_item created = { 0 };
    char stem[EXPLORER_NAME_BYTES];
    size_t slot = (size_t)-1;
    size_t at;

    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < NEW_MENU_COUNT; ++index) {
        if (new_menu_entries[index].kind == kind) {
            entry = &new_menu_entries[index];
            break;
        }
    }
    /* Only the three kinds the New menu offers can be made this way; the
     * rest have no default name or extension to start from. */
    if (entry == NULL) {
        return EXPLORER_STATUS_BAD_INDEX;
    }
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (!items[index].present) {
            slot = index;
            break;
        }
    }
    if (slot == (size_t)-1) {
        return EXPLORER_STATUS_BAD_INDEX;
    }
    /*
     * A new row arrives named "New folder", which a filter for anything
     * else would hide the instant it was made - leaving a rename open on a
     * row that is not on screen.  Making something is a reason to stop
     * filtering, so the box empties and gives the caret to the rename that
     * is about to open, which is where the next keystroke should go.
     */
    clear_search();
    set_search_focus(false);
    unique_stem(entry->default_stem, entry->suffix, stem, sizeof(stem));
    created.kind = kind;
    created.selected = true;
    at = append_literal(created.name, sizeof(created.name), 0U, stem);
    (void)append_literal(created.name, sizeof(created.name), at,
        entry->suffix);
    copy_field(created.type, entry->type_label, sizeof(created.type));
    /* Windows shows the real creation time; there is no filesystem behind
     * this window to have given it one, and "Just now" is at least true. */
    copy_field(created.modified, "Just now", sizeof(created.modified));
    if (kind != EXPLORER_FOLDER) {
        /* Windows leaves a folder's size blank in this view and shows an
         * empty file's as zero, which created.size already is by having
         * been zero-initialized above for a folder. */
        copy_field(created.size, "0 bytes", sizeof(created.size));
    }
    (void)explorer_set_item(slot, &created);
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (index != slot) {
            items[index].selected = false;
        }
    }
    copy_field(rename_stem, stem, sizeof(rename_stem));
    copy_field(rename_suffix, entry->suffix, sizeof(rename_suffix));
    rename_length = 0U;
    while (rename_stem[rename_length] != '\0') {
        ++rename_length;
    }
    rename_active = true;
    rename_index = slot;
    rename_selected = true;
    rename_created = true;
    copy_field(rename_original, created.name, sizeof(rename_original));
    caret_visible = true;
    caret_phase = (uint32_t)(clock_monotonic_ns() / UINT64_C(530000000)) %
        2U;
    open_menu = (size_t)-1;
    menu_hover = (size_t)-1;
    *damage = window_rect;
    return EXPLORER_STATUS_OK;
}

bool explorer_renaming(void)
{
    return rename_active;
}

/* Clears every place's current-ness except one - the same walk a click on a
 * place already made, factored out so a click and "Go to <place>" from the
 * palette run identical code rather than two copies of the same loop. */
static void select_place(size_t index)
{
    for (size_t other = 0U; other < EXPLORER_MAX_PLACES; ++other) {
        places[other].current = other == index;
    }
}

/*
 * Navigating, and remembering it.
 *
 * Everything that changes where you are standing goes through here - a
 * click in the tree, a breadcrumb segment, "Go to" from the palette - so
 * Back has one place to read and no caller has to remember to tell it.
 * Going somewhere new drops whatever Forward had, which is what every
 * back button in every program does.
 */
static void navigate_to(size_t index)
{
    if (index >= EXPLORER_MAX_PLACES || !places[index].present ||
            index == current_place()) {
        select_place(index);
        return;
    }
    select_place(index);
    if (history_at + 1U < history_count) {
        history_count = history_at + 1U;
    }
    if (history_count == EXPLORER_HISTORY) {
        /* Full: drop the oldest.  Sixteen steps back is further than
         * anyone walks, and a stack that refused to move once it filled
         * would stop recording the trip you are actually on. */
        for (size_t scan = 1U; scan < EXPLORER_HISTORY; ++scan) {
            history[scan - 1U] = history[scan];
        }
        --history_count;
    }
    history[history_count] = index;
    ++history_count;
    history_at = history_count - 1U;
}

/*
 * THE CLIPBOARD, which is one slot.
 *
 * That is what a shell with no system clipboard behind it can honestly
 * offer: Copy remembers a row, Cut remembers it and marks it to be moved,
 * and Paste puts it back into the list under a name nothing else is using.
 * Nothing leaves this window - there is no other window in this shell to
 * hand a file to yet, and pretending otherwise would make Copy a lie rather
 * than a limitation.
 */
static void hold_in_clipboard(size_t index, bool cut)
{
    clipboard_item = items[index];
    clipboard_full = true;
    clipboard_cut = cut;
    clipboard_source = cut ? index : (size_t)-1;
}

static void forget_clipboard_row(size_t index)
{
    if (clipboard_source == index) {
        clipboard_source = (size_t)-1;
        clipboard_cut = false;
    }
}

/*
 * Paste.  A cut is a MOVE - the source row goes and the name comes with it,
 * which is why the source is cleared before the new name is worked out.  A
 * copy is a copy, and Windows names one "<name> - Copy", numbering from
 * there if that is taken too.
 */
static bool paste_clipboard(void)
{
    struct explorer_item pasted = clipboard_item;
    const bool moving = clipboard_cut;
    char stem[EXPLORER_NAME_BYTES];
    char suffix[EXPLORER_FIELD_BYTES];
    char base[EXPLORER_NAME_BYTES];
    char unique[EXPLORER_NAME_BYTES];
    size_t slot = (size_t)-1;
    size_t at;

    if (!clipboard_full) {
        return false;
    }
    if (clipboard_cut && clipboard_source < EXPLORER_MAX_ITEMS &&
            items[clipboard_source].present) {
        items[clipboard_source] = (struct explorer_item){ 0 };
    }
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (!items[index].present) {
            slot = index;
            break;
        }
    }
    if (slot == (size_t)-1) {
        return false;
    }
    split_name(pasted.name, stem, sizeof(stem), suffix, sizeof(suffix));
    at = append_literal(base, sizeof(base), 0U, stem);
    if (!clipboard_cut) {
        (void)append_literal(base, sizeof(base), at, " - Copy");
    }
    unique_stem(base, suffix, unique, sizeof(unique));
    at = append_literal(pasted.name, sizeof(pasted.name), 0U, unique);
    (void)append_literal(pasted.name, sizeof(pasted.name), at, suffix);
    pasted.present = true;
    pasted.selected = true;
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        items[index].selected = false;
    }
    items[slot] = pasted;
    queue_action(moving ? EXPLORER_ACTION_MOVE : EXPLORER_ACTION_COPY,
        pasted.kind, clipboard_item.name, pasted.name);
    if (clipboard_cut) {
        /* A cut is spent once it lands; a copy stays on the clipboard, so
         * a second Paste makes a second copy. */
        clipboard_full = false;
        clipboard_cut = false;
        clipboard_source = (size_t)-1;
    }
    return true;
}

/* Opens the inline rename on an existing row - the same editor a new one
 * opens in, with the extension left out of the highlight, and with the old
 * name kept so Escape can put it back. */
static void begin_rename(size_t index)
{
    if (index >= EXPLORER_MAX_ITEMS || !items[index].present) {
        return;
    }
    copy_field(rename_original, items[index].name, sizeof(rename_original));
    split_name(items[index].name, rename_stem, sizeof(rename_stem),
        rename_suffix, sizeof(rename_suffix));
    rename_length = 0U;
    while (rename_stem[rename_length] != '\0') {
        ++rename_length;
    }
    rename_active = true;
    rename_index = index;
    rename_selected = true;
    rename_created = false;
    caret_visible = true;
    caret_phase = (uint32_t)(clock_monotonic_ns() / UINT64_C(530000000)) %
        2U;
}

/*
 * One press of one control on the command bar.
 *
 * A control that carries a chevron opens its menu - or closes it, if it is
 * the one already open, which is how a menu bar behaves and what makes the
 * same button both the way in and the way out.  Everything else acts at
 * once and closes whatever was open.
 */
static enum explorer_status run_command(size_t index, struct ui_rect *damage)
{
    const size_t chosen = selected_item();

    *damage = window_rect;
    if (!command_enabled(index)) {
        return EXPLORER_STATUS_OK;
    }
    if (command_specs[index].menu) {
        open_menu = open_menu == index ? (size_t)-1 : index;
        menu_hover = (size_t)-1;
        return EXPLORER_STATUS_OK;
    }
    open_menu = (size_t)-1;
    menu_hover = (size_t)-1;
    switch (index) {
    case EXPLORER_COMMAND_CUT:
        hold_in_clipboard(chosen, true);
        break;
    case EXPLORER_COMMAND_COPY:
        hold_in_clipboard(chosen, false);
        break;
    case EXPLORER_COMMAND_PASTE:
        (void)paste_clipboard();
        break;
    case EXPLORER_COMMAND_RENAME:
        begin_rename(chosen);
        break;
    case EXPLORER_COMMAND_DELETE:
        if (rename_active && rename_index == chosen) {
            /* The editor cannot outlive the row it is editing. */
            rename_active = false;
            rename_index = (size_t)-1;
        }
        queue_action(EXPLORER_ACTION_DELETE, items[chosen].kind,
            items[chosen].name, "");
        forget_clipboard_row(chosen);
        items[chosen] = (struct explorer_item){ 0 };
        break;
    default:
        break;
    }
    return EXPLORER_STATUS_OK;
}

/* And one row of whichever menu is open.  Every row is an action or a
 * setting, so running one always closes the menu. */
static enum explorer_status run_menu_row(size_t row, struct ui_rect *damage)
{
    const size_t command = open_menu;

    open_menu = (size_t)-1;
    menu_hover = (size_t)-1;
    *damage = window_rect;
    if (command >= EXPLORER_COMMAND_COUNT ||
            row >= menu_row_count(command)) {
        return EXPLORER_STATUS_OK;
    }
    switch (command) {
    case EXPLORER_COMMAND_NEW:
        return explorer_create_item(new_menu_entries[row].kind, damage);
    case EXPLORER_COMMAND_SORT:
        if (row < EXPLORER_SORT_COUNT) {
            sort_column = (enum explorer_sort)row;
        } else {
            sort_descending = row != EXPLORER_SORT_COUNT;
        }
        break;
    case EXPLORER_COMMAND_VIEW:
        tiles_view = row != 0U;
        break;
    default:
        break;
    }
    return EXPLORER_STATUS_OK;
}

/*
 * Select all / none / invert, over what the search box is SHOWING rather
 * than over everything the window holds.  A filtered list that answers
 * "select all" with rows you cannot see is the one thing worse than not
 * offering it - and it is what Windows does too: Ctrl+A in a search result
 * takes the result, not the folder behind it.
 */
static void set_all_items_selected(bool selected)
{
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (item_visible(index)) {
            items[index].selected = selected;
        }
    }
}

static void invert_item_selection(void)
{
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (item_visible(index)) {
            items[index].selected = !items[index].selected;
        }
    }
}

/*
 * Runs one row of the palette's current match list and closes it - every
 * command here is a single action rather than a mode to stay in, so there
 * is nothing a run leaves the palette open FOR.
 */
static enum explorer_status run_palette_command(size_t visible,
    struct ui_rect *damage)
{
    const struct palette_command *command;
    enum explorer_status status = EXPLORER_STATUS_OK;

    *damage = palette_rect();
    palette_open = false;
    if (visible >= palette_match_count) {
        return EXPLORER_STATUS_OK;
    }
    command = &palette_all[palette_matches[visible]];
    switch (command->action) {
    case PALETTE_NEW_FOLDER:
    case PALETTE_NEW_TEXT:
    case PALETTE_NEW_BITMAP: {
        struct ui_rect create_damage;
        static const enum explorer_kind kind_for[3] = {
            EXPLORER_FOLDER, EXPLORER_TEXT, EXPLORER_IMAGE
        };

        status = explorer_create_item(
            kind_for[command->action - PALETTE_NEW_FOLDER],
            &create_damage);
        *damage = join(*damage, create_damage);
        break;
    }
    case PALETTE_SELECT_ALL:
        set_all_items_selected(true);
        *damage = window_rect;
        break;
    case PALETTE_SELECT_NONE:
        set_all_items_selected(false);
        *damage = window_rect;
        break;
    case PALETTE_INVERT_SELECTION:
        invert_item_selection();
        *damage = window_rect;
        break;
    case PALETTE_TOGGLE_VIEW:
        tiles_view = !tiles_view;
        *damage = window_rect;
        break;
    case PALETTE_CLEAR_SEARCH:
        clear_search();
        *damage = window_rect;
        break;
    case PALETTE_GO_TO_PLACE:
        navigate_to(command->place_index);
        *damage = window_rect;
        break;
    default:
        break;
    }
    return status;
}

enum explorer_status explorer_toggle_command_palette(struct ui_rect *damage)
{
    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    if (palette_open) {
        *damage = palette_rect();
        palette_open = false;
        return EXPLORER_STATUS_OK;
    }
    /* Not while a row is mid-rename - the two would be fighting over the
     * same keystrokes and the same caret. */
    if (rename_active) {
        return EXPLORER_STATUS_OK;
    }
    open_menu = (size_t)-1;
    menu_hover = (size_t)-1;
    palette_open = true;
    palette_query[0] = '\0';
    palette_query_length = 0U;
    rebuild_palette_commands();
    rebuild_palette_matches();
    caret_visible = true;
    caret_phase = (uint32_t)(clock_monotonic_ns() / UINT64_C(530000000)) %
        2U;
    *damage = palette_rect();
    return EXPLORER_STATUS_OK;
}

/*
 * Which pointer belongs at a point.
 *
 * The shapes existed before anything asked for them: cursor.c drew thirteen
 * of Windows' fifteen, and the only thing choosing between them was a
 * walkthrough script naming one per scene, which is a picture of a pointer
 * changing rather than a pointer that changes.  A cursor is not a set you
 * look at, it is one mark that answers "what would happen if I pressed
 * here", so the window that knows the answer is the one that has to give
 * it - the same way it already knows what to draw and what a click does.
 *
 * The four the window can genuinely claim:
 *
 *   an EDGE or a CORNER is a resize, and which resize depends on which,
 *   with the corners winning over the sides they meet at because that is
 *   the narrower target and the one you were aiming for;
 *   a TEXT BOX is a caret - the search box, the palette's input, and the
 *   row being renamed, which are exactly the three things in this window
 *   that take typing;
 *   a BREADCRUMB segment is a link, since clicking one goes somewhere;
 *   everything else is the arrow.
 *
 * Nothing here reports Busy or Working in background.  Those two are not
 * about WHERE the pointer is - they say the machine is thinking, which is
 * something only the code doing the thinking can know, so they stay the
 * caller's to set.
 */
enum cursor_kind explorer_cursor_at(struct ui_point point)
{
    const struct ui_rect frame = window_rect;
    const bool near_left = point.x >= (int32_t)frame.x &&
        point.x < (int32_t)(frame.x + EXPLORER_RESIZE_EDGE);
    const bool near_right =
        point.x >= (int32_t)(frame.x + frame.width - EXPLORER_RESIZE_EDGE) &&
        point.x < (int32_t)(frame.x + frame.width);
    const bool near_top = point.y >= (int32_t)frame.y &&
        point.y < (int32_t)(frame.y + EXPLORER_RESIZE_EDGE);
    const bool near_bottom =
        point.y >= (int32_t)(frame.y + frame.height - EXPLORER_RESIZE_EDGE) &&
        point.y < (int32_t)(frame.y + frame.height);

    if (!initialized || !holds(frame, point)) {
        return CURSOR_NORMAL_SELECT;
    }
    /* Corners first: each one overlaps two edges, and the corner is the
     * smaller target, so answering with an edge there would make the
     * corner unhittable. */
    if ((near_left && near_top) || (near_right && near_bottom)) {
        return CURSOR_DIAGONAL_RESIZE_1;
    }
    if ((near_right && near_top) || (near_left && near_bottom)) {
        return CURSOR_DIAGONAL_RESIZE_2;
    }
    if (near_left || near_right) {
        return CURSOR_HORIZONTAL_RESIZE;
    }
    if (near_top || near_bottom) {
        return CURSOR_VERTICAL_RESIZE;
    }
    if (holds(search_rect(), point) && !holds(search_clear_rect(), point)) {
        return CURSOR_TEXT_SELECT;
    }
    if (palette_open && holds(palette_input_rect(), point)) {
        return CURSOR_TEXT_SELECT;
    }
    if (rename_active && holds(item_rect(rename_index), point)) {
        return CURSOR_TEXT_SELECT;
    }
    /* Only a segment that names somewhere the tree can go - see
     * crumb_place().  A link cursor over "Users" would be the pointer
     * making a promise the window cannot keep. */
    for (size_t index = 0U; index < EXPLORER_MAX_CRUMBS; ++index) {
        if (crumb_place(index) != (size_t)-1 &&
                holds(crumb_rect(index), point)) {
            return CURSOR_LINK_SELECT;
        }
    }
    return CURSOR_NORMAL_SELECT;
}

enum explorer_status explorer_focus_search(bool wanted,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    /* Not while a row is mid-rename, for the same reason the palette
     * refuses: two boxes cannot share one caret. */
    if (wanted && rename_active) {
        return EXPLORER_STATUS_OK;
    }
    set_search_focus(wanted);
    *damage = window_rect;
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_clear_search(struct ui_rect *damage)
{
    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    clear_search();
    *damage = window_rect;
    return EXPLORER_STATUS_OK;
}

bool explorer_search_focused(void)
{
    return search_focused;
}

const char *explorer_search_query(void)
{
    return search_query;
}

size_t explorer_visible_item_count(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (item_visible(index)) {
            ++count;
        }
    }
    return count;
}

bool explorer_command_palette_open(void)
{
    return palette_open;
}

enum explorer_status explorer_text_input(char character,
    struct ui_rect *damage)
{
    /* Windows refuses these in a file name; dropped here rather than
     * inserted and refused later, since there is no error dialog for
     * "later" to show. */
    static const char reserved[] = "\\/:*?\"<>|";
    size_t max_stem;

    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    if ((unsigned char)character < 0x20U) {
        return EXPLORER_STATUS_OK;
    }
    if (palette_open) {
        /* No reserved characters here - this is filtering text, not naming
         * a file, so nothing typed into it needs refusing. */
        if (palette_query_length + 1U < sizeof(palette_query)) {
            palette_query[palette_query_length] = character;
            ++palette_query_length;
            palette_query[palette_query_length] = '\0';
            rebuild_palette_matches();
        }
        caret_visible = true;
        *damage = palette_rect();
        return EXPLORER_STATUS_OK;
    }
    /* Filtering text as well, so the same "nothing to refuse" applies.  The
     * whole window repaints rather than the box alone: a keystroke here
     * moves rows, the count in the status bar and what the scroll position
     * means, none of which is inside the search box's own rect. */
    if (search_focused) {
        if (search_query_length + 1U < sizeof(search_query)) {
            search_query[search_query_length] = character;
            ++search_query_length;
            search_query[search_query_length] = '\0';
        }
        caret_visible = true;
        *damage = window_rect;
        return EXPLORER_STATUS_OK;
    }
    if (!rename_active) {
        return EXPLORER_STATUS_OK;
    }
    for (size_t index = 0U; reserved[index] != '\0'; ++index) {
        if (character == reserved[index]) {
            return EXPLORER_STATUS_OK;
        }
    }
    if (rename_selected) {
        rename_length = 0U;
        rename_stem[0] = '\0';
        rename_selected = false;
    }
    /* One byte short of the stem's share of EXPLORER_NAME_BYTES, so the
     * suffix always still fits after it. */
    max_stem = sizeof(rename_stem) - 1U;
    for (size_t index = 0U; rename_suffix[index] != '\0'; ++index) {
        --max_stem;
    }
    if (rename_length + 1U < max_stem) {
        rename_stem[rename_length] = character;
        ++rename_length;
        rename_stem[rename_length] = '\0';
    }
    sync_rename_name();
    caret_visible = true;
    *damage = item_rect(rename_index);
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_key_backspace(struct ui_rect *damage)
{
    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    if (palette_open) {
        if (palette_query_length > 0U) {
            --palette_query_length;
            palette_query[palette_query_length] = '\0';
            rebuild_palette_matches();
        }
        caret_visible = true;
        *damage = palette_rect();
        return EXPLORER_STATUS_OK;
    }
    if (search_focused) {
        if (search_query_length > 0U) {
            --search_query_length;
            search_query[search_query_length] = '\0';
        }
        caret_visible = true;
        *damage = window_rect;
        return EXPLORER_STATUS_OK;
    }
    if (!rename_active) {
        return EXPLORER_STATUS_OK;
    }
    if (rename_selected) {
        /* Backspace on the initial selection clears it, the same as typing
         * over it would - it just leaves nothing behind instead of one
         * character. */
        rename_length = 0U;
        rename_stem[0] = '\0';
        rename_selected = false;
    } else if (rename_length > 0U) {
        --rename_length;
        rename_stem[rename_length] = '\0';
    }
    sync_rename_name();
    caret_visible = true;
    *damage = item_rect(rename_index);
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_key_enter(struct ui_rect *damage)
{
    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    if (palette_open) {
        /* Nothing to run against an empty match list; stay open rather
         * than close on a query that matched nothing. */
        return palette_match_count > 0U ?
            run_palette_command(0U, damage) : EXPLORER_STATUS_OK;
    }
    /* The filter is already live - it narrowed the list on the keystroke,
     * not on Enter - so all Enter has left to do is give the caret back and
     * leave the rows where they are.  Windows commits a search here; there
     * is nothing to commit when there was never a pending state. */
    if (search_focused) {
        set_search_focus(false);
        *damage = address_rect();
        return EXPLORER_STATUS_OK;
    }
    if (!rename_active) {
        return EXPLORER_STATUS_OK;
    }
    /* Windows will not create a nameless file either; Enter on an empty
     * stem is refused and editing continues rather than committing one. */
    if (rename_length == 0U) {
        return EXPLORER_STATUS_OK;
    }
    *damage = item_rect(rename_index);
    if (rename_created) {
        queue_action(EXPLORER_ACTION_CREATE, items[rename_index].kind, "",
            items[rename_index].name);
    } else {
        bool changed = false;

        for (size_t index = 0U;
             rename_original[index] != '\0' ||
                items[rename_index].name[index] != '\0'; ++index) {
            if (rename_original[index] != items[rename_index].name[index]) {
                changed = true;
                break;
            }
        }
        if (changed) {
            queue_action(EXPLORER_ACTION_RENAME, items[rename_index].kind,
                rename_original, items[rename_index].name);
        }
    }
    rename_active = false;
    rename_index = (size_t)-1;
    return EXPLORER_STATUS_OK;
}

bool explorer_take_action(struct explorer_action *action)
{
    if (!action_waiting) {
        return false;
    }
    if (action != NULL) {
        *action = pending_action;
    }
    pending_action = (struct explorer_action){ 0 };
    action_waiting = false;
    return true;
}

enum explorer_status explorer_key_escape(struct ui_rect *damage)
{
    size_t index;

    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    if (palette_open) {
        *damage = palette_rect();
        palette_open = false;
        return EXPLORER_STATUS_OK;
    }
    /*
     * Escape empties the box AND drops the caret, which is the one place
     * this differs from Enter.  Windows clears a search box on Escape and
     * puts the folder back; a filter you have to reach for the mouse to
     * undo is a filter that can trap you looking at four of twenty rows.
     */
    if (search_focused) {
        clear_search();
        set_search_focus(false);
        *damage = window_rect;
        return EXPLORER_STATUS_OK;
    }
    if (!rename_active) {
        return EXPLORER_STATUS_OK;
    }
    index = rename_index;
    *damage = window_rect;
    rename_active = false;
    rename_index = (size_t)-1;
    /*
     * What Escape undoes depends on how the rename started - see
     * rename_created.  A row that only exists because New made it goes with
     * the rename, which is what Windows does to a New folder abandoned
     * before it is named; a row that was already there gets its old name
     * back and stays.
     */
    if (rename_created) {
        (void)explorer_set_item(index, NULL);
    } else if (index < EXPLORER_MAX_ITEMS) {
        copy_field(items[index].name, rename_original,
            sizeof(items[index].name));
    }
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_draw(struct ui_rect damage)
{
    enum explorer_status status;

    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    status = fill(window_rect, damage,
        focused ? border_active : border_inactive);
    if (status == EXPLORER_STATUS_OK) {
        status = draw_caption(damage);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = draw_commands(damage);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = draw_address(damage);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = draw_nav(damage);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = draw_list(damage);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = draw_status(damage);
    }
    if (status == EXPLORER_STATUS_OK) {
        /* Floats over everything else, the way a flyout has to. */
        status = draw_menu(damage);
    }
    if (status == EXPLORER_STATUS_OK) {
        status = draw_command_palette(damage);
    }
    return status;
}

/* ================================================================== INPUT */

enum explorer_status explorer_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const size_t was_item = hover_item;
    const size_t was_place = hover_place;
    const size_t was_command = hover_command;
    const size_t was_crumb = hover_crumb;
    const size_t was_address = hover_address;

    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    hover_item = (size_t)-1;
    hover_place = (size_t)-1;
    hover_command = (size_t)-1;
    hover_crumb = (size_t)-1;
    hover_address = (size_t)-1;
    if (open_menu < EXPLORER_COMMAND_COUNT) {
        /* The menu owns the pointer while it is open - Windows does not
         * hot-track the list underneath a flyout either. */
        const size_t was_menu = menu_hover;
        const size_t rows = menu_row_count(open_menu);

        menu_hover = (size_t)-1;
        for (size_t row = 0U; row < rows; ++row) {
            if (holds(menu_row_rect(row), point)) {
                menu_hover = row;
                break;
            }
        }
        if (was_menu != menu_hover || was_command != (size_t)-1) {
            *damage = join(menu_rect(), commands_rect());
        }
        return EXPLORER_STATUS_OK;
    }
    if (palette_open) {
        const size_t was_palette_hover = palette_hover;

        palette_hover = (size_t)-1;
        for (size_t visible = 0U; visible < palette_match_count &&
             visible < PALETTE_MAX_VISIBLE; ++visible) {
            if (holds(palette_row_rect(visible), point)) {
                palette_hover = visible;
                break;
            }
        }
        if (was_palette_hover != palette_hover) {
            *damage = palette_rect();
        }
        return EXPLORER_STATUS_OK;
    }
    {
        /* The clear mark lights up under the pointer.  It is the only
         * thing on the address bar that reacts to being pointed at, which
         * is how you find out it is a button and not an ornament. */
        const bool was_clear_hot = search_clear_hot;

        search_clear_hot = holds(search_clear_rect(), point);
        if (was_clear_hot != search_clear_hot) {
            *damage = search_rect();
        }
    }
    for (size_t index = 0U; index < EXPLORER_COMMAND_COUNT; ++index) {
        if (command_enabled(index) && holds(command_rect(index), point)) {
            hover_command = index;
            break;
        }
    }
    for (size_t index = 0U; index < 3U; ++index) {
        if (address_slot_enabled(index) &&
                holds(address_slot_rect(index), point)) {
            hover_address = index;
            break;
        }
    }
    for (size_t index = 0U; index < EXPLORER_MAX_CRUMBS; ++index) {
        if (crumb_place(index) != (size_t)-1 &&
                holds(crumb_rect(index), point)) {
            hover_crumb = index;
            break;
        }
    }
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (item_visible(index) && holds(item_rect(index), point)) {
            hover_item = index;
            break;
        }
    }
    for (size_t index = 0U; index < EXPLORER_MAX_PLACES; ++index) {
        if (places[index].present && holds(place_rect(index), point)) {
            hover_place = index;
            break;
        }
    }
    /* The bar and the address strip switch rather than fade - they are
     * buttons, and a button that takes 83 ms to admit the pointer is on it
     * reads as a slow window.  Only the two LISTS cross-fade. */
    if (was_command != hover_command) {
        *damage = join(*damage, commands_rect());
    }
    if (was_crumb != hover_crumb || was_address != hover_address) {
        *damage = join(*damage, address_rect());
    }
    if (was_item != hover_item || was_place != hover_place) {
        /* 83 ms and linear, which is what a XAML BrushTransition is; see
         * ui_motion.h. */
        const uint64_t now = clock_monotonic_ns();

        if (was_item != hover_item) {
            leaving_item = was_item;
            ui_motion_reset(&item_leave_fade, item_fade.value);
            ui_motion_to(&item_leave_fade, 0, UI_MOTION_BRUSH_NS, now);
            ui_motion_reset(&item_fade, 0);
            if (hover_item != (size_t)-1) {
                ui_motion_to(&item_fade, (int32_t)UI_MOTION_ONE,
                    UI_MOTION_BRUSH_NS, now);
            }
        }
        if (was_place != hover_place) {
            leaving_place = was_place;
            ui_motion_reset(&place_leave_fade, place_fade.value);
            ui_motion_to(&place_leave_fade, 0, UI_MOTION_BRUSH_NS, now);
            ui_motion_reset(&place_fade, 0);
            if (hover_place != (size_t)-1) {
                ui_motion_to(&place_fade, (int32_t)UI_MOTION_ONE,
                    UI_MOTION_BRUSH_NS, now);
            }
        }
        *damage = window_rect;
    }
    return EXPLORER_STATUS_OK;
}

/* A row's rectangle, or nothing at all for the index that means none. */
static struct ui_rect item_damage(size_t index)
{
    if (index >= EXPLORER_MAX_ITEMS) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return item_rect(index);
}

static struct ui_rect place_damage(size_t index)
{
    if (index >= EXPLORER_MAX_PLACES) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return place_rect(index);
}

bool explorer_animate(struct ui_rect *damage)
{
    const uint64_t now = clock_monotonic_ns();
    bool moved = false;

    if (damage == NULL || !initialized) {
        return false;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (ui_motion_advance(&item_fade, now, ui_ease_linear)) {
        *damage = join(*damage, item_damage(hover_item));
        moved = true;
    }
    if (ui_motion_advance(&item_leave_fade, now, ui_ease_linear)) {
        *damage = join(*damage, item_damage(leaving_item));
        moved = true;
    }
    if (ui_motion_advance(&place_fade, now, ui_ease_linear)) {
        *damage = join(*damage, place_damage(hover_place));
        moved = true;
    }
    if (ui_motion_advance(&place_leave_fade, now, ui_ease_linear)) {
        *damage = join(*damage, place_damage(leaving_place));
        moved = true;
    }
    if (rename_active || palette_open || search_focused) {
        /*
         * A caret blinks in half-second halves rather than on a running
         * motion: it is fully on or fully off, never partway, so there is
         * nothing here for ui_motion.h's interpolation to do.  Only the
         * PHASE crossing a boundary is a redraw; sampling the clock every
         * frame and redrawing regardless would repaint a steady caret
         * sixty times a second for nothing.  One caret, shared by whichever
         * of the three text boxes is listening - no two of them ever are.
         */
        const uint32_t phase = (uint32_t)(now / UINT64_C(530000000)) % 2U;

        if (phase != caret_phase) {
            caret_phase = phase;
            caret_visible = !caret_visible;
            if (rename_active) {
                *damage = join(*damage, item_damage(rename_index));
            } else if (palette_open) {
                *damage = join(*damage, palette_input_rect());
            } else {
                *damage = join(*damage, search_rect());
            }
            moved = true;
        }
    }
    return moved;
}

bool explorer_animating(void)
{
    return ui_motion_running(&item_fade) ||
        ui_motion_running(&item_leave_fade) ||
        ui_motion_running(&place_fade) ||
        ui_motion_running(&place_leave_fade) ||
        rename_active || palette_open || search_focused;
}

static enum explorer_status pointer_press_inner(struct ui_point point,
    struct ui_rect *damage)
{
    if (open_menu < EXPLORER_COMMAND_COUNT) {
        /* A hit runs the row; a miss anywhere - including back on the
         * button that opened it - just dismisses the menu, which is the one
         * click a menu is allowed to spend on itself. */
        const size_t rows = menu_row_count(open_menu);

        for (size_t row = 0U; row < rows; ++row) {
            if (holds(menu_row_rect(row), point)) {
                return run_menu_row(row, damage);
            }
        }
        *damage = window_rect;
        open_menu = (size_t)-1;
        menu_hover = (size_t)-1;
        return EXPLORER_STATUS_OK;
    }
    if (palette_open) {
        /* A hit runs that command; a miss anywhere else just closes the
         * palette, the same "one click, spent on itself" rule the menus
         * follow - clicking a command is doing something, clicking away
         * from it is changing your mind. */
        for (size_t visible = 0U; visible < palette_match_count &&
             visible < PALETTE_MAX_VISIBLE; ++visible) {
            if (holds(palette_row_rect(visible), point)) {
                return run_palette_command(visible, damage);
            }
        }
        *damage = palette_rect();
        palette_open = false;
        palette_hover = (size_t)-1;
        return EXPLORER_STATUS_OK;
    }
    if (rename_active) {
        /*
         * Inside the editor, a click does nothing - there is no caret to
         * move it to, see the note on draw_name_cell().  Anywhere else
         * resolves the rename first and stops: what the click landed on
         * gets its own effect on the NEXT press, the same way a menu eats
         * the click that dismisses it.  A non-empty name commits, exactly
         * what Enter does; an empty one cancels, exactly what Escape does -
         * Windows would hold you in an error balloon instead, which this
         * shell has no machinery for.
         */
        if (holds(item_rect(rename_index), point)) {
            return EXPLORER_STATUS_OK;
        }
        return rename_length > 0U ? explorer_key_enter(damage) :
            explorer_key_escape(damage);
    }
    /*
     * The search box.  The x clears and keeps the caret - you pressed it to
     * type something else, not to stop typing.  Anywhere else in the box
     * takes focus.
     *
     * A click OUTSIDE it while it holds focus drops the caret and then
     * carries on to whatever was actually clicked, rather than being eaten
     * the way a menu and the palette eat theirs.  Those two are modal and a
     * click away from them is a decision to close them; a text field is not
     * modal, and a list you have to click twice to get a row out of is a
     * list with a bug in it.
     */
    if (holds(search_clear_rect(), point)) {
        clear_search();
        set_search_focus(true);
        *damage = window_rect;
        return EXPLORER_STATUS_OK;
    }
    if (holds(search_rect(), point)) {
        set_search_focus(true);
        *damage = address_rect();
        return EXPLORER_STATUS_OK;
    }
    for (size_t index = 0U; index < EXPLORER_COMMAND_COUNT; ++index) {
        const struct ui_rect box = command_rect(index);

        if (box.width != 0U && holds(box, point)) {
            return run_command(index, damage);
        }
    }
    /* Back, forward and up, over the navigation pane's own history. */
    for (size_t index = 0U; index < 3U; ++index) {
        if (!holds(address_slot_rect(index), point)) {
            continue;
        }
        if (!address_slot_enabled(index)) {
            return EXPLORER_STATUS_OK;
        }
        if (index == 0U) {
            --history_at;
            select_place(history[history_at]);
        } else if (index == 1U) {
            ++history_at;
            select_place(history[history_at]);
        } else {
            navigate_to(parent_place());
        }
        *damage = window_rect;
        return EXPLORER_STATUS_OK;
    }
    for (size_t index = 0U; index < EXPLORER_MAX_CRUMBS; ++index) {
        const size_t place = crumb_place(index);

        if (place != (size_t)-1 && holds(crumb_rect(index), point)) {
            navigate_to(place);
            *damage = window_rect;
            return EXPLORER_STATUS_OK;
        }
    }
    /* A column heading sorts by it, and the one already sorted by reverses.
     * Windows' own header does exactly this. */
    for (size_t index = 0U; index < EXPLORER_SORT_COUNT; ++index) {
        const struct ui_rect box = header_rect(index);

        if (box.width == 0U || !holds(box, point)) {
            continue;
        }
        if ((size_t)sort_column == index) {
            sort_descending = !sort_descending;
        } else {
            sort_column = (enum explorer_sort)index;
            sort_descending = false;
        }
        *damage = window_rect;
        return EXPLORER_STATUS_OK;
    }
    for (size_t which = 0U; which < 2U; ++which) {
        if (holds(view_toggle_rect(which), point)) {
            tiles_view = which == 1U;
            *damage = window_rect;
            return EXPLORER_STATUS_OK;
        }
    }
    for (size_t index = 0U; index < EXPLORER_MAX_ITEMS; ++index) {
        if (!item_visible(index) || !holds(item_rect(index), point)) {
            continue;
        }
        /* A plain click selects one row and clears the rest, which is what a
         * list without a modifier key does. */
        for (size_t other = 0U; other < EXPLORER_MAX_ITEMS; ++other) {
            items[other].selected = other == index;
        }
        *damage = window_rect;
        return EXPLORER_STATUS_OK;
    }
    for (size_t index = 0U; index < EXPLORER_MAX_PLACES; ++index) {
        if (!place_visible(index) || !holds(place_rect(index), point)) {
            continue;
        }
        /* The chevron toggles the branch without navigating; anywhere else
         * on the row navigates without touching it - which is the same
         * split Windows' own tree makes. */
        if (holds(place_chevron_rect(index), point)) {
            places[index].expanded = !places[index].expanded;
            *damage = nav_rect();
            return EXPLORER_STATUS_OK;
        }
        navigate_to(index);
        *damage = window_rect;
        return EXPLORER_STATUS_OK;
    }
    return EXPLORER_STATUS_OK;
}

/*
 * A press, and the one thing that has to happen around every single branch
 * of one: a click anywhere but the search box takes the caret away from it.
 *
 * That is a wrapper rather than another branch in the chain above because
 * the branches each own their damage, and several of them report a rect
 * that does not reach the address bar - clicking a row in the tree reports
 * the tree.  Dropping the caret inside one of those would leave the focus
 * ring painted over a box that is no longer listening until something else
 * happened to repaint that strip.  Joining it here cannot be forgotten by a
 * branch added later.
 */
enum explorer_status explorer_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    bool dropped = false;

    if (damage == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return EXPLORER_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (search_focused && !holds(search_rect(), point) &&
            !holds(search_clear_rect(), point)) {
        set_search_focus(false);
        dropped = true;
    }

    const enum explorer_status status = pointer_press_inner(point, damage);

    if (dropped) {
        *damage = join(*damage, search_rect());
    }
    return status;
}

/* ============================================================== LIFECYCLE */

enum explorer_status explorer_set_frame(struct ui_rect frame)
{
    const uint32_t least_width = EXPLORER_BORDER * 2U + EXPLORER_NAV + 320U;
    const uint32_t least_height = EXPLORER_BORDER * 2U + EXPLORER_CAPTION +
        EXPLORER_COMMANDS + EXPLORER_ADDRESS + EXPLORER_HEADER +
        EXPLORER_ROW + EXPLORER_STATUS;

    if (frame.width < least_width || frame.height < least_height) {
        return EXPLORER_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum explorer_status status;

    if (target == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    status = explorer_set_frame(frame);
    if (status != EXPLORER_STATUS_OK) {
        return status;
    }
    canvas = target;
    open_menu = (size_t)-1;
    menu_hover = (size_t)-1;
    hover_command = (size_t)-1;
    hover_crumb = (size_t)-1;
    hover_address = (size_t)-1;
    clipboard_full = false;
    clipboard_cut = false;
    clipboard_source = (size_t)-1;
    sort_column = EXPLORER_SORT_NAME;
    sort_descending = false;
    tiles_view = false;
    history_count = 0U;
    history_at = 0U;
    rename_active = false;
    rename_index = (size_t)-1;
    rename_selected = false;
    rename_created = false;
    pending_action = (struct explorer_action){ 0 };
    action_waiting = false;
    palette_open = false;
    palette_query[0] = '\0';
    palette_query_length = 0U;
    palette_all_count = 0U;
    palette_match_count = 0U;
    palette_hover = (size_t)-1;
    search_focused = false;
    search_query[0] = '\0';
    search_query_length = 0U;
    search_clear_hot = false;
    caret_visible = true;
    caret_phase = 0U;
    initialized = true;
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_set_item(size_t index,
    const struct explorer_item *item)
{
    if (index >= EXPLORER_MAX_ITEMS) {
        return EXPLORER_STATUS_BAD_INDEX;
    }
    if (item == NULL) {
        items[index] = (struct explorer_item){ 0 };
        return EXPLORER_STATUS_OK;
    }
    if ((size_t)item->kind >= EXPLORER_KIND_COUNT) {
        return EXPLORER_STATUS_BAD_INDEX;
    }
    items[index] = *item;
    items[index].present = true;
    items[index].name[EXPLORER_NAME_BYTES - 1U] = '\0';
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_set_place(size_t index,
    const struct explorer_place *place)
{
    if (index >= EXPLORER_MAX_PLACES) {
        return EXPLORER_STATUS_BAD_INDEX;
    }
    if (place == NULL) {
        places[index] = (struct explorer_place){ 0 };
        return EXPLORER_STATUS_OK;
    }
    places[index] = *place;
    places[index].present = true;
    places[index].label[EXPLORER_NAME_BYTES - 1U] = '\0';
    /* Where the caller says you are standing is where the history starts.
     * Without this the first click in the tree would be the first entry
     * and Back would have nothing behind it to go to. */
    if (places[index].current && history_count == 0U) {
        history[0] = index;
        history_count = 1U;
        history_at = 0U;
    }
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_set_crumb(size_t index, const char *label)
{
    if (index >= EXPLORER_MAX_CRUMBS) {
        return EXPLORER_STATUS_BAD_INDEX;
    }
    copy_field(crumbs[index], label, EXPLORER_NAME_BYTES);
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_set_title(const char *title)
{
    if (title == NULL) {
        return EXPLORER_STATUS_NULL_ARGUMENT;
    }
    copy_field(window_title, title, EXPLORER_NAME_BYTES);
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_set_focus(bool active)
{
    focused = active;
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_set_view(bool tiles)
{
    tiles_view = tiles;
    return EXPLORER_STATUS_OK;
}

enum explorer_status explorer_set_sort(enum explorer_sort column,
    bool descending)
{
    if ((size_t)column >= EXPLORER_SORT_COUNT) {
        return EXPLORER_STATUS_BAD_INDEX;
    }
    sort_column = column;
    sort_descending = descending;
    return EXPLORER_STATUS_OK;
}

/* ============================================================== SELF TEST */

bool explorer_self_test(void)
{
    struct explorer_item probe = { 0 };

    /* Every kind has a mark and a colour, so no row can come out blank. */
    for (size_t kind = 0U; kind < EXPLORER_KIND_COUNT; ++kind) {
        if (kind_glyph[kind] == NULL ||
                !has_icon(kind_glyph[kind], EXPLORER_ROW_ICON)) {
            self_test_failure = "an explorer file kind has no icon";
            return false;
        }
    }
    /* Every icon the command palette's six fixed commands draw, plus its
     * own search mark - a "Go to <place>" entry needs no separate check
     * here since it draws whatever the place's own glyph is, and every
     * present place is required elsewhere to have one. */
    {
        static const char *const palette_icons[] = {
            "folder-plus", "file-plus", "square-check", "square",
            "rotate-ccw", "list", "layout-grid", "search", "x"
        };

        for (size_t index = 0U;
             index < sizeof(palette_icons) / sizeof(palette_icons[0]);
             ++index) {
            if (!has_icon(palette_icons[index], PALETTE_ICON)) {
                self_test_failure = "a command palette icon is missing";
                return false;
            }
        }
    }
    /* The three things the New menu can make, checked at the size it draws
     * them and against the same table explorer_create_item() reads - so a
     * kind the menu offers with no corresponding entry, or one whose icon
     * went missing, fails here instead of silently drawing nothing. */
    for (size_t index = 0U; index < NEW_MENU_COUNT; ++index) {
        if (!has_icon(new_menu_entries[index].glyph, MENU_ICON)) {
            self_test_failure = "a New menu entry has no icon";
            return false;
        }
        if ((size_t)new_menu_entries[index].kind >= EXPLORER_KIND_COUNT) {
            self_test_failure = "a New menu entry names an unknown kind";
            return false;
        }
    }
    /*
     * And every mark the command bar names, at the size it draws it - the
     * failure that would otherwise show up as a silently empty button.
     * Both of Sort's arrows and both of View's marks are checked, not just
     * whichever one the current state happens to be showing.
     */
    {
        static const char *const bar_icons[] = {
            "folder-plus", "scissors", "copy", "clipboard-paste", "pencil",
            "trash-2", "arrow-up", "arrow-down", "list", "layout-grid",
            "chevron-down", "chevron-up", "check", "chevron-right",
            "arrow-left", "arrow-right", "pin", "search", "x"
        };

        for (size_t index = 0U;
             index < sizeof(bar_icons) / sizeof(bar_icons[0]); ++index) {
            if (!has_icon(bar_icons[index], EXPLORER_COMMAND_ICON)) {
                self_test_failure = "a command bar mark is missing";
                return false;
            }
        }
        for (size_t index = 0U; index < EXPLORER_COMMAND_COUNT; ++index) {
            if (command_specs[index].glyph == NULL ||
                    !has_icon(command_specs[index].glyph,
                        EXPLORER_COMMAND_ICON)) {
                self_test_failure = "a command bar mark is missing";
                return false;
            }
            for (size_t row = 0U; row < menu_row_count(index); ++row) {
                const char *glyph = menu_row_glyph(index, row);

                if (menu_row_label(index, row)[0] == '\0') {
                    self_test_failure = "a menu row has no label";
                    return false;
                }
                if (glyph != NULL && !has_icon(glyph, MENU_ICON)) {
                    self_test_failure = "a menu row mark is missing";
                    return false;
                }
            }
            /* A control that carries a chevron must have rows behind it,
             * and one that does not must have none - a menu button that
             * opens an empty panel is the ribbon's failure in miniature. */
            if (command_specs[index].menu !=
                    (menu_row_count(index) != 0U)) {
                self_test_failure = "a command's menu does not match it";
                return false;
            }
        }
    }
    /*
     * NOTHING IS RESAMPLED.  Every box this window draws an illustration or
     * a line mark into has to be a size that artwork exists at, because the
     * drawing code picks the largest cell that FITS and composites it one
     * to one: a box that is not a native size quietly gets a smaller mark
     * floating in it, and a box SMALLER than the smallest cell gets one
     * that overflows.  Both are the class of bug this window was audited
     * for once already; this is what stops it coming back.
     */
    {
        static const uint32_t art_boxes[] = { EXPLORER_ROW_ICON,
            EXPLORER_TREE_ICON, EXPLORER_TILE_ICON, MENU_ICON };
        static const uint32_t line_boxes[] = { EXPLORER_COMMAND_ICON,
            EXPLORER_SORT_MARK, EXPLORER_TREE_MARK, MENU_ICON, MENU_CHECK };

        for (size_t index = 0U;
             index < sizeof(art_boxes) / sizeof(art_boxes[0]); ++index) {
            bool shell_has = false;
            bool art_has = false;

            for (size_t option = 0U; option < SHELL_ICON_SIZES; ++option) {
                shell_has = shell_has ||
                    shell_icon_size[option] == art_boxes[index];
            }
            for (size_t option = 0U; option < EXPLORER_ART_SIZES; ++option) {
                art_has = art_has ||
                    explorer_art_size[option] == art_boxes[index];
            }
            if (!shell_has || !art_has) {
                self_test_failure = "an icon box is not a native art size";
                return false;
            }
        }
        for (size_t index = 0U;
             index < sizeof(line_boxes) / sizeof(line_boxes[0]); ++index) {
            bool found = false;

            for (size_t option = 0U; option < EXPLORER_LUCIDE_SIZES;
                 ++option) {
                found = found ||
                    explorer_lucide_size[option] == line_boxes[index];
            }
            if (!found) {
                self_test_failure = "a mark box is not a native glyph size";
                return false;
            }
        }
    }
    /*
     * The command bar's left group has to fit the narrowest window this
     * will open in.  draw_commands() skips a control that would run past
     * the bar rather than overlapping the right-hand group, so a bar that
     * has quietly grown shows up as missing buttons rather than as
     * anything louder.
     */
    {
        uint32_t total = EXPLORER_COMMAND_GAP * 2U + EXPLORER_BORDER * 2U;

        for (size_t index = 0U; index < EXPLORER_COMMAND_COUNT; ++index) {
            total += command_width(index);
            if (command_specs[index].separator_after) {
                total += EXPLORER_COMMAND_SEPARATOR;
            }
        }
        if (total > EXPLORER_COMMANDS_MIN_WIDTH) {
            self_test_failure = "the command bar is wider than its window";
            return false;
        }
    }
    /*
     * The two sort keys read out of display strings, which is the one place
     * in this file where a wrong answer is invisible: a mis-parsed date
     * does not fail, it just puts a row in the wrong place.
     */
    if (date_key("02/09/2026  14:12") >= date_key("03/09/2026  02:51") ||
            date_key("28/08/2026  11:20") >= date_key("02/09/2026  14:12")) {
        self_test_failure = "the date column sorts out of order";
        return false;
    }
    if (date_key("Just now") != UINT64_MAX ||
            date_key("") != UINT64_MAX) {
        self_test_failure = "an unreadable date did not sort newest";
        return false;
    }
    if (size_key("") != 0U || size_key("512 KB") <= size_key("184 KB") ||
            size_key("7.4 MB") <= size_key("512 KB") ||
            size_key("1.1 MB") >= size_key("18.2 MB")) {
        self_test_failure = "the size column sorts out of order";
        return false;
    }
    /* And the name split a rename and a paste both work from. */
    {
        char stem[EXPLORER_NAME_BYTES];
        char suffix[EXPLORER_FIELD_BYTES];

        split_name("taskbar.c", stem, sizeof(stem), suffix, sizeof(suffix));
        if (!names_match(stem, "taskbar") || !names_match(suffix, ".c")) {
            self_test_failure = "a name split at the wrong place";
            return false;
        }
        split_name("New folder", stem, sizeof(stem), suffix,
            sizeof(suffix));
        if (!names_match(stem, "New folder") || suffix[0] != '\0') {
            self_test_failure = "a name with no extension grew one";
            return false;
        }
        split_name(".gitignore", stem, sizeof(stem), suffix,
            sizeof(suffix));
        if (!names_match(stem, ".gitignore") || suffix[0] != '\0') {
            self_test_failure = "a dotfile was split into an extension";
            return false;
        }
    }
    if (explorer_set_item(EXPLORER_MAX_ITEMS, &probe) !=
            EXPLORER_STATUS_BAD_INDEX) {
        self_test_failure = "explorer accepted an index past the end";
        return false;
    }
    probe.kind = (enum explorer_kind)EXPLORER_KIND_COUNT;
    if (explorer_set_item(0U, &probe) != EXPLORER_STATUS_BAD_INDEX) {
        self_test_failure = "explorer accepted an unknown file kind";
        return false;
    }
    /*
     * The search box's matching, checked against the rule the header
     * promises: case-insensitive, anywhere in the name, and an empty query
     * matching everything.  The last of those is the one worth a test -
     * item_visible() runs it on every row of every frame, and an empty
     * query that failed to match would empty the whole window rather than
     * fail visibly in one place.
     */
    if (!contains_ci("Reports", "")) {
        self_test_failure = "an empty search query hid a row";
        return false;
    }
    if (!contains_ci("Reports", "port") ||
            !contains_ci("Reports", "REPORTS") ||
            !contains_ci("reports", "Rep")) {
        self_test_failure = "the search box refused a name it matches";
        return false;
    }
    if (contains_ci("Reports", "z") ||
            contains_ci("Reports", "reportss")) {
        self_test_failure = "the search box matched a name it should not";
        return false;
    }
    self_test_failure = "";
    return true;
}

const char *explorer_self_test_failure(void)
{
    return self_test_failure;
}
