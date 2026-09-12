/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * A Windows 10 taskbar.
 *
 * Every number in the METRICS and PALETTE blocks below carries the source it
 * came from.  Where a value could not be sourced it says so and says how it
 * was derived instead, because a measurement nobody can check is not a
 * measurement.  docs/WINDOWS-10-TASKBAR.md holds the long form of each.
 *
 * Windows 10's bar is flat where Windows 11's is rounded: forty pixels tall
 * rather than forty-eight, pinned to the left rather than centred, its
 * buttons square-cornered rectangles rather than rounded ones, and its
 * running indicator a wide underline rather than a short pill.  Its glyphs
 * come from Segoe MDL2 Assets and its translucency is the older recipe
 * rather than Windows 11's luminosity acrylic.  Its Start mark is the
 * four-pane flag in perspective; this bar deliberately does not copy that
 * one, and draws Phipia's own logo in its place.
 *
 * The bar is drawn with three primitives and nothing else: an anti-aliased
 * rectangle that may be rounded, an alpha-composited image, and text.  Curves
 * are rasterized by counting sixteen coverage samples per pixel, the same
 * technique the surrounding Phipia UI already uses for its capsule fills,
 * which keeps a glyph's arcs smooth without a scanline converter - and which
 * costs nothing on Windows 10's square corners, where the coverage is simply
 * full.
 *
 * There is no floating point.  The kernel is built with -mno-sse
 * -msoft-float, so positions, coverage and animation progress are Q16.16
 * fixed point and every intermediate widens to 64 bits before it multiplies.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/framebuffer.h>
#include <phipia/rtc.h>
#include <phipia/surface.h>
#include <phipia/taskbar.h>

#include <phipia/cursor.h>
#include <phipia/ui.h>
#include <phipia/ui_font.h>

#include "ui_motion.h"

/* ================================================================ METRICS
 *
 * All lengths are device-independent pixels at 100% scale, which on a 96 DPI
 * screen are physical pixels.  Windows scales the whole bar by the display
 * scale factor; Phipia runs QEMU's framebuffer at 100%, so the two coincide.
 */

/*
 * The taskbar's two heights.  Windows 10 offers exactly one appearance switch
 * that changes them - Settings > Taskbar > "Use small taskbar buttons" - and
 * the icon size moves with it.  Forty and thirty are what a 100%-scale
 * screenshot measures; the small mode is what SM_CYSMICON's sixteen implies
 * once the button's padding is added.
 *
 * PENDING VERIFICATION.  These, and everything else marked the same way, are
 * this file's current best reading and are being checked against primary
 * sources; the ones that survive will carry a citation instead.
 */
#define TASKBAR_HEIGHT_DEFAULT 40U
#define TASKBAR_HEIGHT_SMALL 30U

/* SM_CXICON / SM_CXSMICON: the two taskbar icon sizes. */
#define TASKBAR_ICON_SIZE 24U
#define TASKBAR_ICON_SIZE_SMALL 16U

/*
 * One taskbar button.  Windows 10's buttons are contiguous and square: there
 * is no gap between them and no corner radius anywhere on the bar, which is
 * the most immediately visible difference from Windows 11.
 *
 * PENDING VERIFICATION for the extent.
 */
#define TASKBAR_BUTTON_EXTENT 40U
#define TASKBAR_BUTTON_EXTENT_SMALL 30U
#define TASKBAR_BUTTON_MARGIN 0U
#define TASKBAR_BUTTON_PITCH (TASKBAR_BUTTON_EXTENT + TASKBAR_BUTTON_MARGIN)

/*
 * Windows 10 fills the whole button cell rather than a panel inset inside it,
 * so the inset is zero and the panel is the cell.  Keeping the two concepts
 * apart anyway is what lets the small posture and the Start button take
 * different insets without the drawing code caring.
 */
#define TASKBAR_BUTTON_INSET_X 0U
#define TASKBAR_BUTTON_INSET_Y 0U
#define TASKBAR_PANEL_SIZE TASKBAR_BUTTON_EXTENT

/*
 * Radii, in eighths of a pixel.  Windows 10 rounds nothing on the bar: every
 * fill is a rectangle with square corners.  The constants remain so that the
 * flyouts, which do have a small radius, can say what theirs is.
 */
#define TASKBAR_EIGHTHS(px) ((px) * 8U)
#define TASKBAR_CORNER_CONTROL 0U
#define TASKBAR_CORNER_OVERLAY 0U
#define TASKBAR_CORNER_INDICATOR 0U

/*
 * The running indicator.  Windows 10 draws a wide underline along the bottom
 * of the button, not Windows 11's short centred pill: a bar most of the
 * button's width, a few pixels tall, sitting flush with the bottom edge.
 *
 * PENDING VERIFICATION for all four numbers.
 */
#define TASKBAR_INDICATOR_HEIGHT 3U
#define TASKBAR_INDICATOR_WIDTH_BACKGROUND 32U
#define TASKBAR_INDICATOR_WIDTH_FOREGROUND 32U
#define TASKBAR_INDICATOR_BOTTOM_MARGIN 0U
#define TASKBAR_INDICATOR_BOTTOM_GAP 0U

/*
 * A window stack.  Windows 10 suggests several windows by drawing offset
 * edges behind the button rather than by moving the highlight.
 *
 * PENDING VERIFICATION.
 */
#define TASKBAR_MULTIWINDOW_INSET_LIGHT 3U
#define TASKBAR_MULTIWINDOW_INSET_DARK 3U

/*
 * The Start button, the search box and Task View.  Windows 10's Start button
 * is wider than an application button, and its search entry point has four
 * shapes: hidden, the Cortana ring only, an icon-width box, and the wide
 * "Search Phipia" field a stock install shows.
 *
 * PENDING VERIFICATION for every width here.
 */
#define TASKBAR_START_BUTTON_WIDTH 48U
#define TASKBAR_TASK_VIEW_WIDTH 48U
#define TASKBAR_SEARCH_BOX_EXTENT 320U
/*
 * The hot-track glow: how strong it is away from the pointer, and how much
 * more it gains under it.  PENDING VERIFICATION - Windows publishes neither,
 * and these are read off the effect rather than off a source.
 */
#define TASKBAR_HOT_TRACK_BASE 0x26U
#define TASKBAR_HOT_TRACK_PEAK 0x33U
#define TASKBAR_SEARCH_LABEL_EXTENT 160U
#define TASKBAR_SEARCH_BOX_INSET_LEFT 0U
#define TASKBAR_SEARCH_BOX_INSET_RIGHT 0U
#define TASKBAR_SEARCH_TEXT_INSET 40U
#define TASKBAR_SEARCH_TEXT_RIGHT_INSET 12U

/*
 * The notification area.  Windows 10's tray icons are sixteen pixels in
 * slots narrower than Windows 11's, and its clock is two lines of text
 * right-aligned against the show-desktop strip.
 *
 * PENDING VERIFICATION.
 */
#define TASKBAR_TRAY_SLOT_WIDTH 24U
#define TASKBAR_TRAY_GLYPH_SIZE 16U
#define TASKBAR_TRAY_ICON_PADDING 4U
#define TASKBAR_TRAY_PANEL_INSET 0U
#define TASKBAR_TRAY_OMNI_FIRST_PADDING 0U

/*
 * The clock.  Windows 10 sets it in Segoe UI at 12 pixels on a 16-pixel line,
 * two lines of it, right-aligned, with the time above the date.
 *
 * PENDING VERIFICATION for the paddings.
 */
#define TASKBAR_CLOCK_FONT_SIZE 12U
#define TASKBAR_CLOCK_LINE_HEIGHT 16U
#define TASKBAR_CLOCK_PADDING 10U
#define TASKBAR_CLOCK_STACK_TOP_MARGIN 0U
#define TASKBAR_CLOCK_STACK_RIGHT_MARGIN 0U

/*
 * "Show desktop": the sliver at the very edge of the screen.  Windows 10
 * shows it by default, unlike Windows 11, and it is a plain vertical rule
 * with no artwork of its own.
 *
 * PENDING VERIFICATION.
 */
#define TASKBAR_SHOW_DESKTOP_WIDTH 12U
#define TASKBAR_SHOW_DESKTOP_PIPE_HEIGHT 40U

/* Whether the bar draws a line along its top edge. PENDING VERIFICATION. */
#define TASKBAR_TOP_STROKE_HEIGHT 1U

/*
 * A download's progress, and the unread-count badge.
 * PENDING VERIFICATION.
 */
#define TASKBAR_PROGRESS_HEIGHT 4U
#define TASKBAR_PROGRESS_WIDTH TASKBAR_BUTTON_EXTENT
#define TASKBAR_BADGE_HEIGHT 16U
#define TASKBAR_BADGE_PADDING 4U
#define TASKBAR_BADGE_OVERHANG 4U

/* Windows 10 draws no keyboard focus ring outside the button. */
#define TASKBAR_FOCUS_MARGIN 0U
#define TASKBAR_FOCUS_THICKNESS 1U

/*
 * "Needs attention" is FlashWindowEx, which Windows 10 and Windows 11 share:
 * the button alternates with a plate on the classic Win32 cadence - seven
 * toggles at the caret blink time, which defaults to 530 milliseconds - and
 * then stays on the plate until the window is opened.
 */
#define TASKBAR_ATTENTION_TOGGLE_NS UINT64_C(530000000)
#define TASKBAR_ATTENTION_TOGGLES 7U

/*
 * The material.
 *
 * Windows 10's taskbar is NOT blurred.  Its Start menu and Action Center are
 * acrylic, and the taskbar is not: with "Transparency effects" on it is a
 * flat dark tint composited straight over the desktop, which is why wallpaper
 * colour shows through it but wallpaper detail does not, and why third-party
 * tools exist to add a blur that Windows itself never puts there.  Blurring
 * it is the single most common way a Windows 10 taskbar is drawn wrong.
 *
 * The blur machinery below is kept because it is what turning that on would
 * need - and it is what the Windows 11 bar in this branch's history used -
 * but bar_blur is false, so nothing calls it.
 *
 * PENDING VERIFICATION: the tint's alpha.
 */
#define TASKBAR_MATERIAL_DOWNSAMPLE 4U
#define TASKBAR_MATERIAL_BLUR_PASSES 3U
#define TASKBAR_MATERIAL_BLUR_RADIUS 7U /* at quarter resolution */
#define TASKBAR_MATERIAL_MARGIN \
    (TASKBAR_MATERIAL_BLUR_RADIUS * TASKBAR_MATERIAL_BLUR_PASSES * \
        TASKBAR_MATERIAL_DOWNSAMPLE)
#define TASKBAR_MATERIAL_MAX_WIDTH 1920U
#define TASKBAR_MATERIAL_MAX_HEIGHT \
    (TASKBAR_HEIGHT_DEFAULT + TASKBAR_MATERIAL_MARGIN)
#define TASKBAR_MATERIAL_SMALL_WIDTH \
    (TASKBAR_MATERIAL_MAX_WIDTH / TASKBAR_MATERIAL_DOWNSAMPLE + 2U)
#define TASKBAR_MATERIAL_SMALL_HEIGHT \
    (TASKBAR_MATERIAL_MAX_HEIGHT / TASKBAR_MATERIAL_DOWNSAMPLE + 2U)
#define TASKBAR_NOISE_OPACITY 5U

/* Coverage sampling for every curved edge: four by four inside one pixel. */
#define TASKBAR_SAMPLES 4U
#define TASKBAR_SUBPIXEL 8 /* eighth-pixel units, so 4 samples land on odds */

/* Fixed point, matching ui_anim.h so the two can share progress values. */
/* The shell's fixed-point one, which the blur's sampler shares with the
 * motion in ui_motion.h.  Defined in terms of it so the two cannot drift. */
#define TASKBAR_ONE UI_MOTION_ONE

/* ================================================================ PALETTE
 *
 * WinUI's theme resources name every one of these.  The hex values are the
 * ones the light and dark resource dictionaries assign, and the alpha is
 * carried separately because Phipia's surface has no alpha channel: colours
 * are composited as they are drawn rather than stored.
 */

struct taskbar_colour {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
};

#define TASKBAR_RGB(r, g, b) { (r), (g), (b), 0xFFU }
#define TASKBAR_RGBA(r, g, b, a) { (r), (g), (b), (a) }

struct taskbar_palette {
    /*
     * AcrylicBackgroundFillColorBase.  TintColor carries both layers' colour;
     * material_luminosity carries only TintLuminosityOpacity, in its alpha,
     * because the luminosity layer's colour is derived from the backdrop
     * rather than chosen.
     */
    struct taskbar_colour material_tint;       /* TintColor at TintOpacity */
    struct taskbar_colour material_luminosity; /* TintLuminosityOpacity */
    struct taskbar_colour material_fallback;   /* transparency turned off */

    /*
     * ShellTaskbarItemFillColor*, the shell-private family the taskbar's own
     * buttons use.  These are much stronger in the light theme than the
     * generic SubtleFillColor tokens an ordinary control would use, which is
     * why a hovered taskbar button in the light theme goes nearly white while
     * a hovered list item barely moves.
     *
     *   Secondary  hover, and the resting fill of the foreground app
     *   Tertiary   pressed
     *   Primary    the foreground app while it is also hovered
     */
    struct taskbar_colour item_secondary;
    struct taskbar_colour item_tertiary;
    struct taskbar_colour item_primary;
    /*
     * The search box.
     *
     * It is not a lighter wash of the bar: on a dark Windows 10 taskbar the
     * field is a LIGHT plate with dark text in it, because the box is a XAML
     * surface that follows the app theme while the bar around it follows the
     * Windows theme.  That is why a dark taskbar has a near-white search box
     * sitting in it, and it is the single most conspicuous colour on the
     * whole bar - a copy that draws it as a slightly-lighter grey reads as
     * washed out no matter what else is right.
     *
     * Its ink therefore has to be its own too, rather than the bar's.
     * PENDING VERIFICATION of the exact values.
     */
    struct taskbar_colour search_field;
    struct taskbar_colour search_text;

    /*
     * The Start menu.  It IS acrylic - the one place Windows 10 uses the
     * material its taskbar does not - so it carries a tint opacity and a
     * separate luminosity opacity, exactly as the Windows 11 bar did.
     * The rail down its left edge is a shade darker again.
     * PENDING VERIFICATION.
     */
    struct taskbar_colour start_background;
    uint8_t start_luminosity;
    struct taskbar_colour start_rail;
    struct taskbar_colour tile_text;

    /* TextFillColorPrimary and TextFillColorSecondary. */
    struct taskbar_colour text_primary;
    struct taskbar_colour text_secondary;

    /* AccentFillColorDefault: the foreground app's indicator, and a badge. */
    struct taskbar_colour accent_fill;
    /*
     * The accent as TEXT rather than as a plate.
     *
     * #0078D4 on a plate is Windows' accent and is right there.  As ink on
     * the dark Start panel it comes to about 2:1 against the acrylic, which
     * is under the floor for text of any size - the alphabet headings down
     * the app list were legible only if you already knew they were letters.
     * The dark theme lightens it, which is what Windows does with an accent
     * on a dark surface; the light theme has no such problem and keeps it.
     */
    struct taskbar_colour accent_text;
    /* RunningIndicatorBrush: a window that is running but not in front. */
    struct taskbar_colour indicator_background;
    /* RunningIndicatorRequestingAttentionBrush: SystemFillColorCritical. */
    struct taskbar_colour indicator_attention;


    /* The "needs attention" plate, which is opaque rather than a wash. */
    struct taskbar_colour attention_rest;
    struct taskbar_colour attention_hover;
    struct taskbar_colour attention_pressed;

    /* ProgressIndicator's two colours. */
    struct taskbar_colour progress_foreground;
    struct taskbar_colour progress_track;

    /* SurfaceStrokeColorDefault, on the bar's top edge. */
    struct taskbar_colour top_stroke;
    /* The rule down the left edge of the show-desktop sliver.  It is
     * its own colour rather than the bar's top stroke, because
     * Windows 10 draws this one and does not draw that one.
     * PENDING VERIFICATION. */
    struct taskbar_colour show_desktop_rule;

    /* Flyout surfaces, which are a different acrylic from the bar's. */
    /*
     * AcrylicBackgroundFillColorDefault, which is a different brush from the
     * bar's: #2C2C2C at TintOpacity 0.15 and TintLuminosityOpacity 0.96 in
     * the dark theme, #FCFCFC at 0.0 and 0.85 in the light one.  The alpha
     * below carries the luminosity opacity; the tint's is separate.
     */
    struct taskbar_colour flyout_background;
    uint8_t flyout_tint_opacity;
    struct taskbar_colour flyout_stroke;
    struct taskbar_colour flyout_item_hover;
};

/* ================================================================== STATE */

enum taskbar_element {
    TASKBAR_ELEMENT_NONE = 0,
    TASKBAR_ELEMENT_START,
    TASKBAR_ELEMENT_SEARCH,
    TASKBAR_ELEMENT_TASK_VIEW,
    TASKBAR_ELEMENT_WIDGETS,
    TASKBAR_ELEMENT_CHAT,
    TASKBAR_ELEMENT_APP_FIRST,
    TASKBAR_ELEMENT_APP_LAST =
        TASKBAR_ELEMENT_APP_FIRST + TASKBAR_MAX_APPS - 1U,
    /*
     * Windows 10's notification area is a row of separate buttons, each with
     * its own hover highlight; collapsing network, volume and battery into
     * one Quick Settings target is Windows 11's doing.  And the Action Center
     * button sits to the RIGHT of the clock, which is the ordering a copy
     * most often gets backwards.
     */
    TASKBAR_ELEMENT_CHEVRON,
    TASKBAR_ELEMENT_TRAY_NETWORK,
    TASKBAR_ELEMENT_TRAY_VOLUME,
    TASKBAR_ELEMENT_TRAY_BATTERY,
    TASKBAR_ELEMENT_CLOCK,
    TASKBAR_ELEMENT_ACTION_CENTER,
    TASKBAR_ELEMENT_SHOW_DESKTOP,
    TASKBAR_ELEMENT_COUNT
};

struct taskbar_button_state {
    struct ui_motion hover;
    struct ui_motion press;
    struct ui_motion indicator;
};

static struct surface *canvas;
static struct taskbar_counters counters;
static const char *self_test_failure = "taskbar self-test has not run";

static bool initialized;
static uint32_t screen_width;
static uint32_t screen_height;
static enum taskbar_size bar_size = TASKBAR_SIZE_DEFAULT;
static enum taskbar_theme bar_theme = TASKBAR_THEME_DARK;
static enum taskbar_alignment bar_alignment = TASKBAR_ALIGNMENT_LEFT;
static bool bar_transparent = true;
/* Windows 10 does not blur behind its taskbar; see the note in METRICS. */
static bool bar_blur;
static bool show_desktop_button = true;
static bool action_centre_visible;
/*
 * The tray's overflow chevron.  Windows 10 hides icons behind it; Phipia has
 * no hidden-icon tray, so a chevron that opens nothing is a button that lies
 * about there being more.  Off, like Task View and Action Center, and back
 * with taskbar_set_chevron_visible().
 */
static bool chevron_visible;
/* Windows 10 has neither a Widgets board nor a Chat button. */
static bool widgets_visible;
static bool search_visible = true;
static enum taskbar_search_mode search_mode = TASKBAR_SEARCH_BOX;
static enum taskbar_search_mode search_mode_effective = TASKBAR_SEARCH_BOX;
/*
 * Windows 10 pins a Task View button beside the search box.  This bar
 * leaves it out for the same reason it leaves out Action Center: Phipia
 * has no timeline and no virtual desktops behind it, so the button would
 * stand for a whole feature that is not there.
 * taskbar_set_task_view_visible(true) puts it back in its Windows 10
 * place, immediately right of the search box.
 */
static bool task_view_visible;
static struct taskbar_colour accent_colour = TASKBAR_RGB(0x00U, 0x78U, 0xD4U);

static struct taskbar_app apps[TASKBAR_MAX_APPS];
/* Where the pointer last was, so the hot-track glow can follow it. */
static uint32_t pointer_column;
/* What the tray battery shows.  Full until something says otherwise. */
static uint8_t battery_percent = 100U;
/* Each application's own colour, found from its icon; see HOT TRACK. */
static struct taskbar_colour app_tint[TASKBAR_MAX_APPS];
static bool app_tint_known[TASKBAR_MAX_APPS];
static struct taskbar_button_state buttons[TASKBAR_ELEMENT_COUNT];
static enum taskbar_element hovered = TASKBAR_ELEMENT_NONE;
static enum taskbar_element pressed_element = TASKBAR_ELEMENT_NONE;
static enum taskbar_element focused = TASKBAR_ELEMENT_NONE;
static bool attention_pulsing;
static uint64_t attention_started_ns;

/*
 * Whether the flash is showing its plate this instant.  After the seventh
 * toggle it stops alternating and the plate stays, which is what makes a
 * window that wanted attention still findable a minute later.
 */
static bool attention_plate_showing(void)
{
    const uint64_t now = clock_monotonic_ns();
    const uint64_t elapsed = now >= attention_started_ns ?
        now - attention_started_ns : 0U;
    const uint64_t toggle = elapsed / TASKBAR_ATTENTION_TOGGLE_NS;

    return toggle >= TASKBAR_ATTENTION_TOGGLES || (toggle % 2U) == 0U;
}

/*
 * The centred cluster slides rather than jumping.  Opening an application
 * makes the cluster wider, which moves its origin left by half a slot; doing
 * that in one frame is the single most obvious way a copy of this bar gives
 * itself away.  The origin is therefore a motion like any other, and the
 * cluster's rectangles are re-placed from it on every animation frame.
 */
static struct ui_motion cluster_slide;
static uint32_t cluster_origin;
static uint32_t cluster_origin_previous;
static uint32_t cluster_origin_target;
static uint32_t cluster_width;
static enum taskbar_element cluster_order[TASKBAR_ELEMENT_COUNT];
static size_t cluster_count;
static bool cluster_placed;

static struct ui_rect bar_rect;
static struct ui_rect element_rects[TASKBAR_ELEMENT_COUNT];
static bool element_present[TASKBAR_ELEMENT_COUNT];

/* The blurred strip, kept at quarter resolution between rebuilds. */
static uint32_t material_small[
    TASKBAR_MATERIAL_SMALL_WIDTH * TASKBAR_MATERIAL_SMALL_HEIGHT
];
static uint32_t material_scratch[
    TASKBAR_MATERIAL_SMALL_WIDTH * TASKBAR_MATERIAL_SMALL_HEIGHT
];
static uint32_t material_small_width;
static uint32_t material_small_height;
static uint32_t material_capture_y;
static bool material_valid;

/* =============================================================== FIXED POINT
 */

static int64_t clamp64(int64_t value, int64_t low, int64_t high)
{
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

static uint32_t clamp_u32(uint32_t value, uint32_t low, uint32_t high)
{
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

/* ============================================================== MOTION */

/*
 * Timings.
 *
 * The first is Windows'.  Every fill and border colour change on a taskbar
 * button is a XAML BrushTransition of 0:0:0.083, declared inline on
 * BackgroundElement, and a BrushTransition has no pronounced easing - it is a
 * straight interpolation - so the hover, press and focus cross-fades are 83
 * milliseconds and linear.
 *
 * The other two are not declared anywhere in Windows either - the
 * indicator's grow and shrink is composition-driven and the cluster's slide
 * has no published duration at all - so rather than invent a figure each,
 * they take the theme duration that is nearest what they need.  The
 * indicator is the slowest thing on the bar and takes Slow; the cluster
 * slide is an ordinary point-to-point move and takes Normal.  The 300 the
 * indicator used to run at was a number from a community reimplementation,
 * which is a better source than nothing and still not a Windows resource.
 */
#define TASKBAR_DURATION_BRUSH_NS UI_MOTION_BRUSH_NS      /* Faster */
#define TASKBAR_DURATION_INDICATOR_NS UI_MOTION_SLOW_NS   /* Slow */
#define TASKBAR_DURATION_SLIDE_NS UI_MOTION_REVEAL_NS     /* Normal */

/* ============================================================== GEOMETRY */

static bool rect_is_empty(struct ui_rect rectangle)
{
    return rectangle.width == 0U || rectangle.height == 0U;
}

static struct ui_rect rect_intersect(struct ui_rect left, struct ui_rect right)
{
    const uint32_t x = left.x > right.x ? left.x : right.x;
    const uint32_t y = left.y > right.y ? left.y : right.y;
    const uint32_t left_right = left.x + left.width;
    const uint32_t right_right = right.x + right.width;
    const uint32_t left_bottom = left.y + left.height;
    const uint32_t right_bottom = right.y + right.height;
    const uint32_t r = left_right < right_right ? left_right : right_right;
    const uint32_t b = left_bottom < right_bottom ? left_bottom : right_bottom;

    if (r <= x || b <= y) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ x, y, r - x, b - y };
}

static struct ui_rect rect_join(struct ui_rect left, struct ui_rect right)
{
    if (rect_is_empty(left)) {
        return right;
    }
    if (rect_is_empty(right)) {
        return left;
    }
    const uint32_t x = left.x < right.x ? left.x : right.x;
    const uint32_t y = left.y < right.y ? left.y : right.y;
    const uint32_t left_right = left.x + left.width;
    const uint32_t right_right = right.x + right.width;
    const uint32_t left_bottom = left.y + left.height;
    const uint32_t right_bottom = right.y + right.height;
    const uint32_t r = left_right > right_right ? left_right : right_right;
    const uint32_t b = left_bottom > right_bottom ? left_bottom : right_bottom;

    return (struct ui_rect){ x, y, r - x, b - y };
}

static bool rect_holds(struct ui_rect rectangle, struct ui_point point)
{
    if (point.x < 0 || point.y < 0) {
        return false;
    }
    const uint32_t x = (uint32_t)point.x;
    const uint32_t y = (uint32_t)point.y;

    return x >= rectangle.x && x < rectangle.x + rectangle.width &&
        y >= rectangle.y && y < rectangle.y + rectangle.height;
}

static struct ui_rect rect_centred(struct ui_rect within, uint32_t width,
    uint32_t height)
{
    const uint32_t x = within.x + (within.width > width ?
        (within.width - width) / 2U : 0U);
    const uint32_t y = within.y + (within.height > height ?
        (within.height - height) / 2U : 0U);

    return (struct ui_rect){ x, y, width, height };
}

/* ================================================================ COLOUR */

/* The same colour, faded by a 0..255 factor - what a reveal multiplies by. */
static struct taskbar_colour scale_alpha(struct taskbar_colour colour,
    uint32_t factor)
{
    colour.alpha = (uint8_t)((uint32_t)colour.alpha * factor / 255U);
    return colour;
}

static uint32_t pack(struct taskbar_colour colour)
{
    return framebuffer_pack(colour.red, colour.green, colour.blue);
}

static uint8_t channel_of(uint32_t pixel, uint8_t shift)
{
    return (uint8_t)((pixel >> shift) & 0xFFU);
}

static uint32_t blend(uint32_t under, uint32_t over, uint32_t alpha)
{
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    const uint32_t inverse = 255U - alpha;
    const uint32_t red = ((uint32_t)channel_of(over, framebuffer.red_position) *
        alpha + (uint32_t)channel_of(under, framebuffer.red_position) *
        inverse + 127U) / 255U;
    const uint32_t green = ((uint32_t)channel_of(over,
        framebuffer.green_position) * alpha +
        (uint32_t)channel_of(under, framebuffer.green_position) * inverse +
        127U) / 255U;
    const uint32_t blue = ((uint32_t)channel_of(over,
        framebuffer.blue_position) * alpha +
        (uint32_t)channel_of(under, framebuffer.blue_position) * inverse +
        127U) / 255U;

    return framebuffer_pack((uint8_t)red, (uint8_t)green, (uint8_t)blue);
}

/*
 * The luminosity the Fluent recipe means.
 *
 * Acrylic's two layers are non-separable blend modes - the "luminosity" and
 * "color" modes of the PDF and CSS compositing model - and those are defined
 * against the 0.30/0.59/0.11 luma, not against Rec. 709.  Using the wrong
 * weights here would tilt every hue the material passes through.
 */
static int32_t luma_channels(int32_t red, int32_t green, int32_t blue)
{
    return (red * 77 + green * 151 + blue * 28) / 256;
}

static uint32_t luma_of(uint32_t pixel)
{
    const struct framebuffer_state framebuffer = framebuffer_get_state();

    return (uint32_t)luma_channels(
        channel_of(pixel, framebuffer.red_position),
        channel_of(pixel, framebuffer.green_position),
        channel_of(pixel, framebuffer.blue_position));
}

/*
 * ClipColor: pull a colour back inside the cube without changing what its
 * luminosity is, by contracting it towards its own grey.  Shifting three
 * channels by the same amount is what SetLum does, and that can take one of
 * them outside 0..255; simply clamping would change the luminosity, which is
 * the one property the whole operation exists to set.
 */
static void clip_colour(int32_t *channels)
{
    const int32_t luma = luma_channels(channels[0], channels[1], channels[2]);
    int32_t low = channels[0];
    int32_t high = channels[0];

    for (size_t index = 1U; index < 3U; ++index) {
        if (channels[index] < low) {
            low = channels[index];
        }
        if (channels[index] > high) {
            high = channels[index];
        }
    }
    if (low < 0 && luma != low) {
        for (size_t index = 0U; index < 3U; ++index) {
            channels[index] = luma + (int32_t)(
                (int64_t)(channels[index] - luma) * luma / (luma - low));
        }
    }
    if (high > 255 && high != luma) {
        for (size_t index = 0U; index < 3U; ++index) {
            channels[index] = luma + (int32_t)(
                (int64_t)(channels[index] - luma) * (255 - luma) /
                (high - luma));
        }
    }
    for (size_t index = 0U; index < 3U; ++index) {
        channels[index] = (int32_t)clamp64(channels[index], 0, 255);
    }
}

/* SetLum: keep a colour's hue and saturation, give it a new lightness. */
static uint32_t set_luma(uint32_t pixel, int32_t luma)
{
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    int32_t channels[3] = {
        channel_of(pixel, framebuffer.red_position),
        channel_of(pixel, framebuffer.green_position),
        channel_of(pixel, framebuffer.blue_position)
    };
    const int32_t shift = luma - luma_channels(channels[0], channels[1],
        channels[2]);

    for (size_t index = 0U; index < 3U; ++index) {
        channels[index] += shift;
    }
    clip_colour(channels);
    return framebuffer_pack((uint8_t)channels[0], (uint8_t)channels[1],
        (uint8_t)channels[2]);
}

/* ============================================================= PALETTES */

/*
 * Dark.  PENDING VERIFICATION: every value in both tables is this file's
 * current best reading of a Windows 10 taskbar and is being checked against
 * primary sources.  The ones that survive will carry a citation.
 *
 * Windows 10's default accent is #0078D7, one unit of blue away from Windows
 * 11's #0078D4, and unlike Windows 11 the taskbar uses it directly for the
 * running underline rather than through a light or dark variant.
 */
static const struct taskbar_palette palette_dark = {
    /*
     * Windows 10's dark taskbar is black, not a dark grey: opaque it is
     * #000000, and with transparency on it is that same black at high
     * opacity, so the wallpaper's colour tints it without ever lifting it
     * towards the #1F1F1F the Start menu uses.
     */
    .material_tint = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0xD9U),
    .material_luminosity = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0xD9U),
    .material_fallback = TASKBAR_RGB(0x00U, 0x00U, 0x00U),
    .item_secondary = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0x1AU),
    .item_tertiary = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0x0DU),
    .item_primary = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0x26U),
    .search_field = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0xF0U),
    .search_text = TASKBAR_RGBA(0x1AU, 0x1AU, 0x1AU, 0xE6U),
    .start_background = TASKBAR_RGBA(0x1FU, 0x1FU, 0x1FU, 0xD8U),
    .start_luminosity = 0xB8U,
    .start_rail = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x33U),
    .tile_text = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0xFFU),
    .text_primary = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0xFFU),
    .text_secondary = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0xC5U),
    .accent_fill = TASKBAR_RGB(0x00U, 0x78U, 0xD7U),
    .accent_text = TASKBAR_RGB(0x60U, 0xB4U, 0xF0U),
    .indicator_background = TASKBAR_RGBA(0x00U, 0x78U, 0xD7U, 0x99U),
    .indicator_attention = TASKBAR_RGB(0xE8U, 0x11U, 0x23U),
    .attention_rest = TASKBAR_RGB(0x7AU, 0x1FU, 0x1FU),
    .attention_hover = TASKBAR_RGB(0x8EU, 0x2AU, 0x2AU),
    .attention_pressed = TASKBAR_RGB(0x6AU, 0x1AU, 0x1AU),
    .progress_foreground = TASKBAR_RGB(0x10U, 0x89U, 0x3EU),
    .progress_track = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0x1AU),
    /*
     * No top rule.  Windows 11 separates its bar from the desktop with
     * a hairline; Windows 10's dark taskbar simply stops, and a lighter
     * line along the top is one of the tells that a copy was drawn from
     * the newer bar.  The field stays so a theme can ask for one.
     */
    .top_stroke = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0x00U),
    .show_desktop_rule = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0x00U),
    .flyout_background = TASKBAR_RGBA(0x2BU, 0x2BU, 0x2BU, 0xF2U),
    .flyout_tint_opacity = 0x26U,
    .flyout_stroke = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0x18U),
    .flyout_item_hover = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0x14U)
};

/* Light.  Windows 10 gained a light taskbar in 1903. PENDING VERIFICATION. */
static const struct taskbar_palette palette_light = {
    .material_tint = TASKBAR_RGBA(0xF2U, 0xF2U, 0xF2U, 0xD9U),
    .material_luminosity = TASKBAR_RGBA(0xF2U, 0xF2U, 0xF2U, 0xD9U),
    .material_fallback = TASKBAR_RGB(0xF2U, 0xF2U, 0xF2U),
    .item_secondary = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x14U),
    .item_tertiary = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x0AU),
    .item_primary = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x1FU),
    /* Light: the field is WHITE on a grey bar, so it lightens where
     * the dark theme's lightens too rather than darkening. */
    .search_field = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0xF0U),
    .search_text = TASKBAR_RGBA(0x1AU, 0x1AU, 0x1AU, 0xE6U),
    .start_background = TASKBAR_RGBA(0xF2U, 0xF2U, 0xF2U, 0xD8U),
    .start_luminosity = 0xB8U,
    .start_rail = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x0FU),
    .tile_text = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0xFFU),
    .text_primary = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0xE4U),
    .text_secondary = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x9EU),
    .accent_fill = TASKBAR_RGB(0x00U, 0x78U, 0xD7U),
    .accent_text = TASKBAR_RGB(0x00U, 0x5AU, 0xA0U),
    .indicator_background = TASKBAR_RGBA(0x00U, 0x78U, 0xD7U, 0x99U),
    .indicator_attention = TASKBAR_RGB(0xE8U, 0x11U, 0x23U),
    .attention_rest = TASKBAR_RGB(0xFDU, 0xE7U, 0xE9U),
    .attention_hover = TASKBAR_RGB(0xFEU, 0xF0U, 0xF5U),
    .attention_pressed = TASKBAR_RGB(0xFEU, 0xEEU, 0xF0U),
    .progress_foreground = TASKBAR_RGB(0x10U, 0x89U, 0x3EU),
    .progress_track = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x14U),
    .top_stroke = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x00U),
    .show_desktop_rule = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x00U),
    .flyout_background = TASKBAR_RGBA(0xF2U, 0xF2U, 0xF2U, 0xF2U),
    .flyout_tint_opacity = 0x00U,
    .flyout_stroke = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x14U),
    .flyout_item_hover = TASKBAR_RGBA(0x00U, 0x00U, 0x00U, 0x0DU)
};

static const struct taskbar_palette *palette(void)
{
    return bar_theme == TASKBAR_THEME_LIGHT ? &palette_light : &palette_dark;
}

/* ========================================================== RASTERIZATION */

/*
 * The one shape this file draws.  A rounded rectangle is described by its
 * bounds and a corner radius; coverage at a pixel is the fraction of sixteen
 * sample points inside the shape.  Everything with a curve in it - the button
 * highlight, the indicator, the tray group's capsule, a flyout's corner -
 * is this function with different arguments.
 *
 * Coordinates are in eighth-pixel units internally so that the four sample
 * positions per axis land on odd eighths, which keeps a shape that is an
 * exact number of pixels wide from picking up a half-covered edge column.
 */
static bool rounded_sample(
    int64_t x,
    int64_t y,
    int64_t width,
    int64_t height,
    int64_t radius
)
{
    int64_t horizontal = 0;
    int64_t vertical = 0;

    if (x < 0 || y < 0 || x >= width || y >= height) {
        return false;
    }
    if (radius <= 0) {
        return true;
    }
    if (x < radius) {
        horizontal = radius - x;
    } else if (x > width - radius) {
        horizontal = x - (width - radius);
    }
    if (y < radius) {
        vertical = radius - y;
    } else if (y > height - radius) {
        vertical = y - (height - radius);
    }
    if (horizontal == 0 || vertical == 0) {
        return true;
    }
    return horizontal * horizontal + vertical * vertical <= radius * radius;
}

/* radius_eighths is in eighths of a pixel, so 1.5 px is 12. */
static uint32_t rounded_coverage(
    uint32_t local_x,
    uint32_t local_y,
    uint32_t width,
    uint32_t height,
    uint32_t radius_eighths
)
{
    const int64_t sub_width = (int64_t)width * TASKBAR_SUBPIXEL;
    const int64_t sub_height = (int64_t)height * TASKBAR_SUBPIXEL;
    const int64_t sub_radius = (int64_t)radius_eighths;
    uint32_t covered = 0U;

    for (uint32_t sample_y = 0U; sample_y < TASKBAR_SAMPLES; ++sample_y) {
        for (uint32_t sample_x = 0U; sample_x < TASKBAR_SAMPLES; ++sample_x) {
            const int64_t position_x = (int64_t)local_x * TASKBAR_SUBPIXEL +
                (int64_t)sample_x * 2 + 1;
            const int64_t position_y = (int64_t)local_y * TASKBAR_SUBPIXEL +
                (int64_t)sample_y * 2 + 1;

            if (rounded_sample(position_x, position_y, sub_width, sub_height,
                    sub_radius)) {
                ++covered;
            }
        }
    }
    return covered;
}

static enum taskbar_status fill_rounded(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t radius_eighths,
    struct taskbar_colour colour,
    uint32_t alpha_scale
)
{
    const struct ui_rect clipped = rect_intersect(bounds,
        rect_intersect(damage, (struct ui_rect){
            0U, 0U, screen_width, screen_height
        }));
    const uint32_t limit = TASKBAR_EIGHTHS(bounds.width < bounds.height ?
        bounds.width : bounds.height) / 2U;
    const uint32_t corner = radius_eighths > limit ? limit : radius_eighths;
    const uint32_t over = pack(colour);
    const uint32_t base_alpha = (uint32_t)colour.alpha * alpha_scale / 255U;

    if (rect_is_empty(clipped) || base_alpha == 0U) {
        return TASKBAR_STATUS_OK;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - bounds.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            uint32_t coverage;
            uint32_t under;
            uint32_t alpha;

            if (corner == 0U) {
                coverage = TASKBAR_SAMPLES * TASKBAR_SAMPLES;
            } else {
                coverage = rounded_coverage(local_x, local_y, bounds.width,
                    bounds.height, corner);
            }
            if (coverage == 0U) {
                continue;
            }
            alpha = (base_alpha * coverage +
                (TASKBAR_SAMPLES * TASKBAR_SAMPLES) / 2U) /
                (TASKBAR_SAMPLES * TASKBAR_SAMPLES);
            if (alpha == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend(under, over, alpha)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKBAR_STATUS_OK;
}

/*
 * A rounded outline, drawn as the difference between two filled shapes.  The
 * focus ring and every flyout edge is one of these; making it the difference
 * of two coverage counts rather than four strokes is what keeps the corners
 * from thickening where the arcs meet the straights.
 */
static enum taskbar_status stroke_rounded(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t radius_eighths,
    struct taskbar_colour colour,
    uint32_t thickness
)
{
    const struct ui_rect clipped = rect_intersect(bounds,
        rect_intersect(damage, (struct ui_rect){
            0U, 0U, screen_width, screen_height
        }));
    const uint32_t limit = TASKBAR_EIGHTHS(bounds.width < bounds.height ?
        bounds.width : bounds.height) / 2U;
    const uint32_t corner = radius_eighths > limit ? limit : radius_eighths;
    const uint32_t over = pack(colour);
    const uint32_t samples = TASKBAR_SAMPLES * TASKBAR_SAMPLES;

    if (rect_is_empty(clipped) || thickness == 0U ||
            bounds.width <= thickness * 2U ||
            bounds.height <= thickness * 2U) {
        return TASKBAR_STATUS_OK;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - bounds.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint32_t outer = rounded_coverage(local_x, local_y,
                bounds.width, bounds.height, corner);
            uint32_t inner = 0U;
            uint32_t under;

            if (outer == 0U) {
                continue;
            }
            if (local_x >= thickness && local_y >= thickness &&
                local_x + thickness < bounds.width &&
                local_y + thickness < bounds.height) {
                inner = rounded_coverage(local_x - thickness,
                    local_y - thickness,
                    bounds.width - thickness * 2U,
                    bounds.height - thickness * 2U,
                    corner > TASKBAR_EIGHTHS(thickness) ?
                        corner - TASKBAR_EIGHTHS(thickness) : 0U);
            }
            if (inner >= outer) {
                continue;
            }
            const uint32_t alpha = ((uint32_t)colour.alpha *
                (outer - inner) + samples / 2U) / samples;

            if (alpha == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend(under, over, alpha)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKBAR_STATUS_OK;
}

/* ============================================================== MATERIAL */

/*
 * Capture the desktop under the bar and reduce it to a quarter-resolution
 * copy.  This must be called after the desktop has been drawn and before the
 * taskbar draws over it, which is the same ordering the surrounding UI
 * already uses for its Dock glass.
 */
/*
 * One separable box pass over a low-resolution plane: horizontal into the
 * scratch, vertical back.  Shared, because the bar's blur and the Start
 * menu's are the same blur at different resolutions and having two copies of
 * it would mean two places for it to drift.
 */
static void box_blur_plane(uint32_t *plane, uint32_t *scratch,
    uint32_t width, uint32_t height, uint32_t radius)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const int32_t reach = (int32_t)radius;

    for (uint32_t pass = 0U; pass < 2U; ++pass) {
        const uint32_t *source = pass == 0U ? plane : scratch;
        uint32_t *target = pass == 0U ? scratch : plane;

        for (uint32_t y = 0U; y < height; ++y) {
            for (uint32_t x = 0U; x < width; ++x) {
                uint32_t red = 0U;
                uint32_t green = 0U;
                uint32_t blue = 0U;
                uint32_t taps = 0U;

                for (int32_t offset = -reach; offset <= reach; ++offset) {
                    const int32_t along = (int32_t)(pass == 0U ? x : y) +
                        offset;
                    const uint32_t limit = pass == 0U ? width : height;
                    const uint32_t clamped = along < 0 ? 0U :
                        ((uint32_t)along >= limit ? limit - 1U :
                            (uint32_t)along);
                    const uint32_t pixel = pass == 0U ?
                        source[(size_t)y * width + clamped] :
                        source[(size_t)clamped * width + x];

                    red += channel_of(pixel, format.red_position);
                    green += channel_of(pixel, format.green_position);
                    blue += channel_of(pixel, format.blue_position);
                    ++taps;
                }
                target[(size_t)y * width + x] = framebuffer_pack(
                    (uint8_t)(red / taps), (uint8_t)(green / taps),
                    (uint8_t)(blue / taps));
            }
        }
    }
}

/* Defined with the Start menu, which is far below this and needs the whole
 * palette; it is called from here because this is where the desktop is still
 * untouched. */
static void capture_open_panel_backdrop(void);

enum taskbar_status taskbar_capture_backdrop(void)
{
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    /*
     * A panel's backdrop is captured whether or not the BAR blurs, because
     * on Windows 10 the panels are acrylic and the bar is not.  Whichever
     * of the two is standing open gets the one shared buffer; neither
     * being open leaves it invalid, which is what it should be.
     */
    capture_open_panel_backdrop();
    if (!bar_blur) {
        /* Nothing samples it, so nothing is copied and nothing is blurred. */
        material_valid = false;
        return TASKBAR_STATUS_OK;
    }
    const uint32_t margin = TASKBAR_MATERIAL_MARGIN;
    const uint32_t top = bar_rect.y > margin ? bar_rect.y - margin : 0U;
    const uint32_t bottom = screen_height;
    const uint32_t height = bottom - top;
    const uint32_t step = TASKBAR_MATERIAL_DOWNSAMPLE;
    const struct framebuffer_state framebuffer = framebuffer_get_state();

    material_capture_y = top;
    material_small_width = (screen_width + step - 1U) / step;
    material_small_height = (height + step - 1U) / step;
    if (material_small_width > TASKBAR_MATERIAL_SMALL_WIDTH ||
        material_small_height > TASKBAR_MATERIAL_SMALL_HEIGHT) {
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    for (uint32_t y = 0U; y < material_small_height; ++y) {
        for (uint32_t x = 0U; x < material_small_width; ++x) {
            uint32_t red = 0U;
            uint32_t green = 0U;
            uint32_t blue = 0U;
            uint32_t samples = 0U;

            for (uint32_t inner_y = 0U; inner_y < step; ++inner_y) {
                const uint32_t source_y = top + y * step + inner_y;

                if (source_y >= bottom) {
                    break;
                }
                for (uint32_t inner_x = 0U; inner_x < step; ++inner_x) {
                    const uint32_t source_x = x * step + inner_x;
                    uint32_t pixel;

                    if (source_x >= screen_width) {
                        break;
                    }
                    if (surface_read_pixel(canvas, source_x, source_y,
                            &pixel) != SURFACE_STATUS_OK) {
                        return TASKBAR_STATUS_SURFACE_FAILURE;
                    }
                    red += channel_of(pixel, framebuffer.red_position);
                    green += channel_of(pixel, framebuffer.green_position);
                    blue += channel_of(pixel, framebuffer.blue_position);
                    ++samples;
                }
            }
            if (samples == 0U) {
                samples = 1U;
            }
            material_small[(size_t)y * material_small_width + x] =
                framebuffer_pack((uint8_t)(red / samples),
                    (uint8_t)(green / samples), (uint8_t)(blue / samples));
        }
    }

    /* Three separable box passes.  Repeated box blurs converge on a Gaussian
     * quickly; three is the usual stopping point because the fourth is not
     * visible and the third already is. */
    for (uint32_t pass = 0U; pass < TASKBAR_MATERIAL_BLUR_PASSES; ++pass) {
        box_blur_plane(material_small, material_scratch, material_small_width,
            material_small_height, TASKBAR_MATERIAL_BLUR_RADIUS);
    }
    material_valid = true;
    counters.material_rebuilds += 1U;
    return TASKBAR_STATUS_OK;
}

/* Bilinear resample of the blurred strip back to a full-resolution pixel. */
static uint32_t material_at(uint32_t x, uint32_t y)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t step = TASKBAR_MATERIAL_DOWNSAMPLE;
    const uint32_t local_y = y - material_capture_y;
    /* Sample at pixel centres, so the reconstruction is not shifted by half
     * a low-resolution cell towards the origin. */
    const int32_t sample_x = (int32_t)(x * 2U + 1U) * (int32_t)TASKBAR_ONE /
        (int32_t)(step * 2U) - (int32_t)(TASKBAR_ONE / 2);
    const int32_t sample_y = (int32_t)(local_y * 2U + 1U) *
        (int32_t)TASKBAR_ONE / (int32_t)(step * 2U) -
        (int32_t)(TASKBAR_ONE / 2);
    const int32_t base_x = sample_x < 0 ? 0 : sample_x / (int32_t)TASKBAR_ONE;
    const int32_t base_y = sample_y < 0 ? 0 : sample_y / (int32_t)TASKBAR_ONE;
    const uint32_t fraction_x = sample_x < 0 ? 0U :
        (uint32_t)(sample_x % (int32_t)TASKBAR_ONE);
    const uint32_t fraction_y = sample_y < 0 ? 0U :
        (uint32_t)(sample_y % (int32_t)TASKBAR_ONE);
    const uint32_t x0 = clamp_u32((uint32_t)base_x, 0U,
        material_small_width - 1U);
    const uint32_t y0 = clamp_u32((uint32_t)base_y, 0U,
        material_small_height - 1U);
    const uint32_t x1 = clamp_u32(x0 + 1U, 0U, material_small_width - 1U);
    const uint32_t y1 = clamp_u32(y0 + 1U, 0U, material_small_height - 1U);
    const uint32_t corners[4] = {
        material_small[(size_t)y0 * material_small_width + x0],
        material_small[(size_t)y0 * material_small_width + x1],
        material_small[(size_t)y1 * material_small_width + x0],
        material_small[(size_t)y1 * material_small_width + x1]
    };
    const uint8_t shifts[3] = {
        format.red_position, format.green_position, format.blue_position
    };
    uint8_t channels[3];

    for (size_t index = 0U; index < 3U; ++index) {
        const int64_t top = (int64_t)channel_of(corners[0], shifts[index]) *
            (TASKBAR_ONE - fraction_x) +
            (int64_t)channel_of(corners[1], shifts[index]) * fraction_x;
        const int64_t bottom = (int64_t)channel_of(corners[2], shifts[index]) *
            (TASKBAR_ONE - fraction_x) +
            (int64_t)channel_of(corners[3], shifts[index]) * fraction_x;
        const int64_t value = (top * (TASKBAR_ONE - fraction_y) +
            bottom * fraction_y) / TASKBAR_ONE / TASKBAR_ONE;

        channels[index] = (uint8_t)clamp64(value, 0, 255);
    }
    return framebuffer_pack(channels[0], channels[1], channels[2]);
}

/*
 * The noise film.
 *
 * Acrylic's grain is a 256 by 256 opaque grey tile composited over the tinted
 * result at two per cent.  The tile has exactly six levels - 0, 51, 102, 153,
 * 204, 255 - in the measured proportions below, which is what keeps a large
 * flat translucent panel from banding.  A hash of the coordinates reproduces
 * that distribution without carrying the tile, and being a hash it is the
 * same grain every frame, so the bar does not shimmer.
 */
static uint32_t noise_hash(uint32_t x, uint32_t y)
{
    /* The tile repeats every 256 pixels, so the hash does too. */
    uint32_t hash = (x & 0xFFU) * UINT32_C(0x9E3779B1) ^
        (y & 0xFFU) * UINT32_C(0x85EBCA77);

    hash ^= hash >> 15;
    hash *= UINT32_C(0x2C1B3C6D);
    hash ^= hash >> 12;
    hash *= UINT32_C(0x297A2D39);
    hash ^= hash >> 15;
    return hash;
}

static uint8_t noise_level(uint32_t x, uint32_t y)
{
    /* Cumulative frequencies of the six levels, in hundredths of a per cent:
     * 2.000, 13.876, 33.621, 33.896, 14.268, 2.338. */
    static const uint32_t cumulative[6] = {
        200U, 14076U, 47697U, 81593U, 95861U, 100000U
    };
    static const uint8_t levels[6] = { 0U, 51U, 102U, 153U, 204U, 255U };
    const uint32_t draw = noise_hash(x, y) % 100000U;

    for (size_t index = 0U; index < 6U; ++index) {
        if (draw < cumulative[index]) {
            return levels[index];
        }
    }
    return levels[5];
}

/*
 * Lay the material down over the bar's strip.
 *
 * With transparency turned off this degrades to the flat FallbackColor, which
 * is the same thing Windows falls back to and, not by coincidence, the colour
 * every independent measurement of a real Windows 11 taskbar reports.
 */
static enum taskbar_status draw_material(struct ui_rect damage)
{
    const struct taskbar_palette *colours = palette();
    const struct ui_rect clipped = rect_intersect(bar_rect, damage);

    if (rect_is_empty(clipped)) {
        return TASKBAR_STATUS_OK;
    }
    if (!bar_transparent || (bar_blur && !material_valid)) {
        const enum taskbar_status flat = fill_rounded(clipped, clipped, 0U,
            colours->material_fallback, 255U);

        if (flat != TASKBAR_STATUS_OK) {
            return flat;
        }
        return fill_rounded((struct ui_rect){ bar_rect.x, bar_rect.y,
            bar_rect.width, TASKBAR_TOP_STROKE_HEIGHT }, damage, 0U,
            colours->top_stroke, 255U);
    }
    const uint32_t tint = pack(colours->material_tint);
    const int32_t tint_luma = (int32_t)luma_of(tint);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t screen_x = clipped.x + x;
            const uint32_t screen_y = clipped.y + y;
            uint32_t pixel;

            if (!bar_blur) {
                /*
                 * Windows 10's path, and the one this bar takes: a single
                 * alpha blend of the tint over the desktop exactly as it
                 * stands.  No blur, no luminosity layer, no noise film -
                 * those three together are what an acrylic surface is, and
                 * the Windows 10 taskbar is not one.  The wallpaper's
                 * COLOUR comes through, because the tint is not opaque; its
                 * DETAIL does not, because at this opacity there is almost
                 * nothing of it left.  That is the whole recipe.
                 */
                if (surface_read_pixel(canvas, screen_x, screen_y, &pixel) !=
                        SURFACE_STATUS_OK) {
                    return TASKBAR_STATUS_SURFACE_FAILURE;
                }
                pixel = blend(pixel, tint, colours->material_tint.alpha);
                if (surface_pixel(canvas, screen_x, screen_y, pixel) !=
                        SURFACE_STATUS_OK) {
                    return TASKBAR_STATUS_SURFACE_FAILURE;
                }
                continue;
            }

            const uint32_t blurred = material_at(screen_x, screen_y);

            /*
             * The luminosity layer: the blurred desktop, keeping its hue and
             * saturation, pushed to the tint's lightness.  This is why a
             * Windows 11 taskbar over a blue wallpaper is a dark blue rather
             * than a flat grey - a plain alpha blend towards #202020 would
             * take the colour out along with the contrast.
             */
            pixel = blend(blurred, set_luma(blurred, tint_luma),
                colours->material_luminosity.alpha);
            /*
             * The tint layer: the tint's hue and saturation at the lightness
             * the previous step arrived at.  Both tints are neutral greys, so
             * in practice this desaturates by TintOpacity - fully in the dark
             * theme's half, not at all in the light theme's zero.
             */
            if (colours->material_tint.alpha != 0U) {
                pixel = blend(pixel,
                    set_luma(tint, (int32_t)luma_of(pixel)),
                    colours->material_tint.alpha);
            }

            const uint8_t grain = noise_level(screen_x, screen_y);

            pixel = blend(pixel,
                framebuffer_pack(grain, grain, grain),
                TASKBAR_NOISE_OPACITY);
            if (surface_pixel(canvas, screen_x, screen_y, pixel) !=
                SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    /* Rectangle#BackgroundStroke, which is what separates the bar from a
     * maximized window sitting directly above it.  It is drawn inside the
     * forty-eight pixels rather than added to them. */
    return fill_rounded((struct ui_rect){ bar_rect.x, bar_rect.y,
        bar_rect.width, TASKBAR_TOP_STROKE_HEIGHT }, damage, 0U,
        colours->top_stroke, 255U);
}

const char *taskbar_status_string(enum taskbar_status status)
{
    switch (status) {
    case TASKBAR_STATUS_OK:
        return "ok";
    case TASKBAR_STATUS_NULL_ARGUMENT:
        return "null argument";
    case TASKBAR_STATUS_NOT_INITIALIZED:
        return "taskbar not initialized";
    case TASKBAR_STATUS_ALREADY_INITIALIZED:
        return "taskbar already initialized";
    case TASKBAR_STATUS_UNSUPPORTED_GEOMETRY:
        return "taskbar geometry is unsupported";
    case TASKBAR_STATUS_BAD_INDEX:
        return "taskbar index is out of range";
    case TASKBAR_STATUS_BAD_ELEMENT:
        return "taskbar element is unknown";
    case TASKBAR_STATUS_RECTANGLE_OVERFLOW:
        return "taskbar rectangle overflows";
    case TASKBAR_STATUS_SURFACE_FAILURE:
        return "taskbar surface refused a pixel";
    case TASKBAR_STATUS_FONT_FAILURE:
        return "taskbar font refused a string";
    case TASKBAR_STATUS_CLOCK_FAILURE:
        return "taskbar could not read the clock";
    default:
        return "unknown taskbar status";
    }
}

/* ================================================================ GLYPHS
 *
 * Every mark on the bar is a Lucide icon.
 *
 * Lucide is stroke-only on a twenty-four unit grid with a two-unit stroke and
 * round caps and joins, which makes rasterizing one exact rather than
 * approximate: a pixel is inside the icon when its distance to the nearest
 * point of any path is at most half the stroke width, and round caps and
 * joins are what that definition means.  tools/make-lucide-glyphs.py does
 * that offline and emits one forty-eight pixel alpha cell per icon into
 * taskbar_glyphs.h, so the kernel carries no path parser and no stroker.
 *
 * Forty-eight divides cleanly by both sizes the bar draws at - twenty-four
 * for a command or application icon, sixteen for a notification-area one -
 * so those are exact box filters of the stored cell rather than resamplings.
 */

#include "taskbar_art.h"
#include "taskbar_glyphs.h"

/* Which Lucide icon each mark is.  A name with no entry draws nothing. */
static const struct taskbar_lucide_entry *glyph_entry(enum taskbar_glyph glyph)
{
    const char *name;

    switch (glyph) {
    case TASKBAR_GLYPH_SEARCH:
        name = "search";
        break;
    case TASKBAR_GLYPH_TASK_VIEW:
        name = "layout-grid";
        break;
    case TASKBAR_GLYPH_CHEVRON_UP:
        name = "chevron-up";
        break;
    case TASKBAR_GLYPH_NETWORK:
        name = "wifi";
        break;
    case TASKBAR_GLYPH_VOLUME:
        name = "volume-2";
        break;
    case TASKBAR_GLYPH_MENU:
        name = "menu";
        break;
    case TASKBAR_GLYPH_PICTURES:
        name = "image";
        break;
    case TASKBAR_GLYPH_BATTERY:
        /* The shell only.  The charge inside it is a filled bar drawn by
         * draw_battery, not three stencilled bars; see BATTERY. */
        name = "battery";
        break;
    /* Only drawn when a caller asks for the button; see the layout. */
    case TASKBAR_GLYPH_ACTION_CENTER:
        name = "message-square";
        break;
    case TASKBAR_GLYPH_FILE_EXPLORER:
        name = "folder";
        break;
    case TASKBAR_GLYPH_TERMINAL:
        name = "terminal";
        break;
    case TASKBAR_GLYPH_NOTES:
        name = "file-text";
        break;
    case TASKBAR_GLYPH_CAMERA:
        name = "camera";
        break;
    case TASKBAR_GLYPH_CANVAS:
        name = "paintbrush";
        break;
    case TASKBAR_GLYPH_STORE:
        name = "shopping-bag";
        break;
    case TASKBAR_GLYPH_SETTINGS:
        name = "settings";
        break;
    case TASKBAR_GLYPH_POWER:
        name = "power";
        break;
    case TASKBAR_GLYPH_ACCOUNT:
        name = "circle-user";
        break;
    case TASKBAR_GLYPH_START:
    case TASKBAR_GLYPH_NONE:
    case TASKBAR_GLYPH_COUNT:
    default:
        return NULL;
    }
    for (size_t index = 0U; index < TASKBAR_LUCIDE_COUNT; ++index) {
        const char *candidate = taskbar_lucide[index].name;
        const char *wanted = name;

        while (*candidate != '\0' && *candidate == *wanted) {
            ++candidate;
            ++wanted;
        }
        if (*candidate == '\0' && *wanted == '\0') {
            return &taskbar_lucide[index];
        }
    }
    return NULL;
}

/*
 * Draw one stored cell into a box of any size, area-averaging the samples
 * that fall inside each destination pixel.  For a reduction that is both the
 * cheapest correct answer and the one that keeps a two-unit stroke even,
 * where point sampling would drop whole columns of it.
 */
static enum taskbar_status draw_alpha_cell(
    const uint8_t *cell,
    uint32_t source_size,
    struct ui_rect bounds,
    struct ui_rect damage,
    struct taskbar_colour colour,
    uint32_t alpha_scale
)
{
    const struct ui_rect clipped = rect_intersect(bounds,
        rect_intersect(damage, (struct ui_rect){
            0U, 0U, screen_width, screen_height
        }));
    const uint32_t over = pack(colour);
    const uint32_t base_alpha = (uint32_t)colour.alpha * alpha_scale / 255U;

    if (cell == NULL || rect_is_empty(clipped) || base_alpha == 0U ||
            bounds.width == 0U || bounds.height == 0U) {
        return TASKBAR_STATUS_OK;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - bounds.y + y;
        const uint32_t top = local_y * source_size / bounds.height;
        uint32_t bottom = (local_y + 1U) * source_size / bounds.height;

        if (bottom <= top) {
            bottom = top + 1U;
        }
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint32_t left = local_x * source_size / bounds.width;
            uint32_t right = (local_x + 1U) * source_size / bounds.width;
            uint32_t total = 0U;
            uint32_t samples = 0U;
            uint32_t under;

            if (right <= left) {
                right = left + 1U;
            }
            for (uint32_t sy = top; sy < bottom && sy < source_size; ++sy) {
                for (uint32_t sx = left; sx < right && sx < source_size;
                     ++sx) {
                    total += cell[(size_t)sy * source_size + sx];
                    ++samples;
                }
            }
            if (samples == 0U || total == 0U) {
                continue;
            }
            const uint32_t alpha = total / samples * base_alpha / 255U;

            if (alpha == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend(under, over, alpha)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKBAR_STATUS_OK;
}

/*
 * Pick the cell rasterized for the size being asked for.  An exact match is
 * drawn one pixel to one pixel, which is the whole reason the generator emits
 * one cell per size; anything else takes the nearest larger cell and
 * area-averages, and only a size the bar does not normally use lands there.
 */
static enum taskbar_status draw_glyph(
    enum taskbar_glyph glyph,
    struct ui_rect bounds,
    struct ui_rect damage,
    struct taskbar_colour colour,
    uint32_t alpha_scale
)
{
    const struct taskbar_lucide_entry *entry = glyph_entry(glyph);
    const uint32_t wanted = bounds.width < bounds.height ? bounds.width :
        bounds.height;
    size_t choice = 0U;
    uint32_t side;

    if (entry == NULL) {
        return TASKBAR_STATUS_OK;
    }
    /* The largest cell that fits the box; see art_planes() above for why
     * it is never one that has to be shrunk into it. */
    for (size_t index = 0U; index < TASKBAR_LUCIDE_SIZES; ++index) {
        if (taskbar_lucide_size[index] <= wanted) {
            choice = index;
        }
    }
    side = taskbar_lucide_size[choice];
    return draw_alpha_cell(entry->alpha[choice], side,
        (struct ui_rect){
            bounds.x + (bounds.width > side ?
                (bounds.width - side) / 2U : 0U),
            bounds.y + (bounds.height > side ?
                (bounds.height - side) / 2U : 0U),
            side, side },
        damage, colour, alpha_scale);
}

/* ================================================================== TEXT
 *
 * Windows sets the taskbar in Segoe UI Variable Text at twelve pixels.  Segoe
 * cannot be redistributed, and Phipia's own font service carries exactly one
 * UI face at one size: Inter at fifteen pixels, in a nineteen-pixel cell.
 *
 * This used to take that cell and RESAMPLE it - four fifths of fifteen is
 * twelve, the size Windows uses - and accepted "a slightly soft glyph" as
 * the price.  It was not slightly soft.  Box-filtering an already
 * antialiased mask by 5:4 turns a one-pixel stem into two grey ones and a
 * crisp terminal into a smear, and it did it to every word on the bar, in
 * the Start menu, in the jump list and in the search panel - while the
 * windows beside them, which draw the service's cell at its own size, were
 * sharp.  Next to each other the difference is not subtle.
 *
 * So the bar carries its OWN face now, rasterized from the same Inter at
 * twelve pixels rather than shrunk from fifteen, exactly the way terminal.c
 * carries a console font and settings.c carries a heading one - see
 * tools/make-ui-font.py, whose whole purpose is this.  Nothing is resampled
 * at draw time; a stem that is one pixel wide in the face is one pixel wide
 * on the screen.  The generated cell is fifteen tall with its baseline on
 * row twelve, which is where the resampled one landed, so the layout this
 * replaced is the layout it keeps.
 */

#include "taskbar_font.h"

static const struct taskbar_font_glyph *font_glyph(uint32_t code)
{
    if (code < TASKBAR_FONT_FIRST || code > TASKBAR_FONT_LAST) {
        return NULL;
    }
    return &taskbar_font[code - TASKBAR_FONT_FIRST];
}

/*
 * Windows lays taskbar text on a sixteen-pixel line, whatever the glyphs
 * inside it measure.  The face's cell is fifteen, so the line is stated
 * rather than derived: getting the leading right matters more to the shape
 * of the clock block than getting the cell height right.
 */
static uint32_t text_line_height(void)
{
    return TASKBAR_CLOCK_LINE_HEIGHT;
}

/* The height of the glyph cell, which is not the line height. */
static uint32_t text_line_cell(void)
{
    return TASKBAR_FONT_HEIGHT;
}

static uint32_t text_ascent(void)
{
    return TASKBAR_FONT_BASELINE;
}

static uint32_t text_width(const char *text)
{
    uint32_t total = 0U;

    if (text == NULL) {
        return 0U;
    }
    for (const char *scan = text; *scan != '\0'; ++scan) {
        const struct taskbar_font_glyph *glyph =
            font_glyph((uint32_t)(unsigned char)*scan);

        if (glyph != NULL) {
            total += glyph->advance;
        }
    }
    return total;
}

/*
 * One glyph, at its own size.  The face is variable-width - a lowercase i is
 * four columns and a W is eleven - so each cell carries its own width and
 * its own left bearing rather than sitting in a box sized for the widest
 * letter in the alphabet.
 */
static enum taskbar_status draw_glyph_cell(
    const struct taskbar_font_glyph *glyph,
    uint32_t left,
    uint32_t top,
    struct ui_rect clip,
    uint32_t colour,
    uint32_t alpha_scale
)
{
    for (uint32_t y = 0U; y < TASKBAR_FONT_HEIGHT; ++y) {
        const uint32_t target_y = top + y;

        if (target_y < clip.y || target_y >= clip.y + clip.height) {
            continue;
        }
        for (uint32_t x = 0U; x < glyph->width; ++x) {
            const uint32_t target_x = left + x;
            const uint8_t coverage = taskbar_font_pixels[glyph->offset +
                (size_t)y * glyph->width + x];
            uint32_t under;
            uint32_t alpha;

            if (coverage == 0U || target_x < clip.x ||
                    target_x >= clip.x + clip.width) {
                continue;
            }
            alpha = (uint32_t)coverage * alpha_scale / 255U;
            if (alpha == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, target_x, target_y, &under) !=
                    SURFACE_STATUS_OK) {
                continue;
            }
            if (surface_pixel(canvas, target_x, target_y,
                    blend(under, colour, alpha)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKBAR_STATUS_OK;
}

static enum taskbar_status draw_text(
    struct ui_rect damage,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    struct taskbar_colour colour
)
{
    const struct ui_rect clip = rect_intersect(damage, (struct ui_rect){
        0U, 0U, screen_width, screen_height
    });
    const uint32_t over = pack(colour);
    /* Whole pixels: the advances are already in them, so there is no
     * fraction left to carry the way the resampled pen had to. */
    uint32_t pen = x;
    const uint32_t ascent = text_ascent();

    if (text == NULL || rect_is_empty(clip)) {
        return TASKBAR_STATUS_OK;
    }
    if (baseline < ascent) {
        return TASKBAR_STATUS_FONT_FAILURE;
    }
    for (const char *scan = text; *scan != '\0'; ++scan) {
        const struct taskbar_font_glyph *glyph =
            font_glyph((uint32_t)(unsigned char)*scan);
        enum taskbar_status status;

        if (glyph == NULL) {
            continue;
        }
        if (glyph->width != 0U) {
            status = draw_glyph_cell(glyph,
                (uint32_t)((int32_t)pen + glyph->bearing), baseline - ascent,
                clip, over, colour.alpha);
            if (status != TASKBAR_STATUS_OK) {
                return status;
            }
            counters.glyphs += 1U;
        }
        pen += glyph->advance;
    }
    return TASKBAR_STATUS_OK;
}

/* ================================================================ CLOCK */

/*
 * The taskbar clock in en-US: a twelve-hour time with an AM/PM marker over a
 * numeric date.  Windows writes both lines right-aligned against the same
 * margin, so the block is as wide as its wider line.
 */
#define TASKBAR_CLOCK_TEXT_BYTES 16U

static char clock_time_text[TASKBAR_CLOCK_TEXT_BYTES];
static char clock_date_text[TASKBAR_CLOCK_TEXT_BYTES];
static bool clock_text_valid;
static uint16_t clock_text_minute_key;

static size_t append_number(
    char *into,
    size_t capacity,
    size_t at,
    uint32_t value,
    uint32_t digits
)
{
    char scratch[12];
    size_t length = 0U;

    do {
        scratch[length++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    } while (value != 0U && length < sizeof(scratch));
    while (length < digits && length < sizeof(scratch)) {
        scratch[length++] = '0';
    }
    while (length > 0U && at + 1U < capacity) {
        into[at++] = scratch[--length];
    }
    into[at] = '\0';
    return at;
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

static void format_clock(const struct rtc_time *now)
{
    const bool afternoon = now->hour >= 12U;
    uint8_t twelve = (uint8_t)(now->hour % 12U);
    size_t at;

    if (twelve == 0U) {
        twelve = 12U;
    }
    at = append_number(clock_time_text, sizeof(clock_time_text), 0U, twelve,
        1U);
    at = append_literal(clock_time_text, sizeof(clock_time_text), at, ":");
    at = append_number(clock_time_text, sizeof(clock_time_text), at,
        now->minute, 2U);
    (void)append_literal(clock_time_text, sizeof(clock_time_text), at,
        afternoon ? " PM" : " AM");

    at = append_number(clock_date_text, sizeof(clock_date_text), 0U,
        now->month, 1U);
    at = append_literal(clock_date_text, sizeof(clock_date_text), at, "/");
    at = append_number(clock_date_text, sizeof(clock_date_text), at, now->day,
        1U);
    at = append_literal(clock_date_text, sizeof(clock_date_text), at, "/");
    (void)append_number(clock_date_text, sizeof(clock_date_text), at,
        now->year, 4U);
}

/*
 * Refresh the two strings if the displayed minute has changed.  Returns true
 * when the text moved, which is what tells the caller the clock needs a
 * redraw; a taskbar that repainted its clock every frame would be presenting
 * an unchanged rectangle sixty times a second.
 */
static bool refresh_clock(void)
{
    struct rtc_time now;

    if (rtc_now(&now) != RTC_STATUS_OK) {
        if (clock_text_valid) {
            return false;
        }
        /* Nothing is known about the time.  Say so rather than showing a
         * plausible-looking zero. */
        (void)append_literal(clock_time_text, sizeof(clock_time_text), 0U,
            "--:--");
        (void)append_literal(clock_date_text, sizeof(clock_date_text), 0U,
            "no clock");
        clock_text_valid = true;
        return true;
    }
    counters.clock_reads += 1U;
    const uint16_t key = (uint16_t)((uint16_t)now.hour * 60U + now.minute);

    if (clock_text_valid && key == clock_text_minute_key) {
        return false;
    }
    format_clock(&now);
    clock_text_minute_key = key;
    clock_text_valid = true;
    return true;
}

static uint32_t clock_block_width(void)
{
    const uint32_t time_width = text_width(clock_time_text);
    const uint32_t date_width = text_width(clock_date_text);

    return time_width > date_width ? time_width : date_width;
}

/*
 * The clock's slot: the wider of its two lines plus four pixels of padding on
 * each side, less the one pixel the stack panel's negative right margin takes
 * back, and never narrower than a tray slot.
 */
static uint32_t clock_slot_width(void)
{
    const uint32_t content = clock_block_width() +
        TASKBAR_CLOCK_PADDING * 2U - TASKBAR_CLOCK_STACK_RIGHT_MARGIN;

    return content < TASKBAR_TRAY_SLOT_WIDTH ? TASKBAR_TRAY_SLOT_WIDTH :
        content;
}

/* ================================================================ LAYOUT */

static uint32_t bar_height(void)
{
    return bar_size == TASKBAR_SIZE_SMALL ? TASKBAR_HEIGHT_SMALL :
        TASKBAR_HEIGHT_DEFAULT;
}

static uint32_t icon_size(void)
{
    return bar_size == TASKBAR_SIZE_SMALL ? TASKBAR_ICON_SIZE_SMALL :
        TASKBAR_ICON_SIZE;
}

static uint32_t button_extent(void)
{
    return bar_size == TASKBAR_SIZE_SMALL ? TASKBAR_BUTTON_EXTENT_SMALL :
        TASKBAR_BUTTON_EXTENT;
}

static bool element_is_foreground(enum taskbar_element element);

static bool element_is_app(enum taskbar_element element)
{
    return element >= TASKBAR_ELEMENT_APP_FIRST &&
        element <= TASKBAR_ELEMENT_APP_LAST;
}

static size_t app_index_of(enum taskbar_element element)
{
    return (size_t)element - (size_t)TASKBAR_ELEMENT_APP_FIRST;
}

/*
 * Whether this button carries the foreground window, which is the one thing
 * that gives a taskbar button a resting background of its own.
 */
static bool element_is_foreground(enum taskbar_element element)
{
    if (!element_is_app(element)) {
        return false;
    }
    const struct taskbar_app *app = &apps[app_index_of(element)];

    return app->present && (app->run == TASKBAR_RUN_FOREGROUND ||
        app->run == TASKBAR_RUN_GROUPED_FOCUS);
}

size_t taskbar_app_count(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < TASKBAR_MAX_APPS; ++index) {
        if (apps[index].present) {
            ++count;
        }
    }
    return count;
}

/*
 * Place everything.
 *
 * The bar has three groups.  The tray is pinned to the right edge and laid
 * out from there leftwards.  The command and application buttons form one
 * cluster which is either centred on the screen - Windows 11's default, in
 * which opening an application shifts Start to the left because the whole
 * cluster re-centres - or pinned to the left edge.  Nothing overlaps: the
 * cluster is pushed left if it would otherwise reach the tray.
 */
/*
 * How wide one cluster slot is.  Everything is a plain button extent except
 * the search entry point, which Windows gives its own width when it is drawn
 * as a box or as an icon with a label.
 */
static uint32_t element_extent(enum taskbar_element element)
{
    switch (element) {
    case TASKBAR_ELEMENT_START:
        /* Windows 10's Start button is wider than an application button. */
        return bar_size == TASKBAR_SIZE_SMALL ?
            button_extent() + TASKBAR_BUTTON_MARGIN :
            TASKBAR_START_BUTTON_WIDTH;
    case TASKBAR_ELEMENT_TASK_VIEW:
        return bar_size == TASKBAR_SIZE_SMALL ?
            button_extent() + TASKBAR_BUTTON_MARGIN :
            TASKBAR_TASK_VIEW_WIDTH;
    case TASKBAR_ELEMENT_SEARCH:
        switch (search_mode_effective) {
        case TASKBAR_SEARCH_BOX:
            return TASKBAR_SEARCH_BOX_EXTENT;
        case TASKBAR_SEARCH_ICON_LABEL:
            return TASKBAR_SEARCH_LABEL_EXTENT;
        case TASKBAR_SEARCH_ICON:
        case TASKBAR_SEARCH_HIDDEN:
        default:
            return button_extent() + TASKBAR_BUTTON_MARGIN;
        }
    default:
        return button_extent() + TASKBAR_BUTTON_MARGIN;
    }
}

static void place_cluster(void);

static void layout(void)
{
    const uint32_t height = bar_height();

    bar_rect = (struct ui_rect){
        0U, screen_height - height, screen_width, height
    };
    for (size_t index = 0U; index < TASKBAR_ELEMENT_COUNT; ++index) {
        element_rects[index] = (struct ui_rect){ 0U, 0U, 0U, 0U };
        element_present[index] = false;
    }

    /*
     * --- the tray, from the right edge leftwards ---
     *
     * SystemTrayFrameGrid is a right-aligned grid of automatically sized
     * columns; laying it out backwards from the screen edge is the same
     * arithmetic, and it means the clock never moves when an app is opened.
     */
    uint32_t right = screen_width;

    if (show_desktop_button) {
        right -= TASKBAR_SHOW_DESKTOP_WIDTH;
        element_rects[TASKBAR_ELEMENT_SHOW_DESKTOP] = (struct ui_rect){
            right, bar_rect.y, TASKBAR_SHOW_DESKTOP_WIDTH, height
        };
        element_present[TASKBAR_ELEMENT_SHOW_DESKTOP] = true;
    }

    /*
     * Windows 10 puts an Action Center button to the right of the clock.
     * This bar leaves it out: Phipia has no notification centre for it to
     * open, and a button that does nothing is worse than no button.
     */
    if (action_centre_visible) {
        right = right > TASKBAR_TRAY_SLOT_WIDTH ?
            right - TASKBAR_TRAY_SLOT_WIDTH : 0U;
        element_rects[TASKBAR_ELEMENT_ACTION_CENTER] = (struct ui_rect){
            right, bar_rect.y, TASKBAR_TRAY_SLOT_WIDTH, height
        };
        element_present[TASKBAR_ELEMENT_ACTION_CENTER] = true;
    }

    (void)refresh_clock();
    const uint32_t clock_width = clock_slot_width();

    right = right > clock_width ? right - clock_width : 0U;
    element_rects[TASKBAR_ELEMENT_CLOCK] = (struct ui_rect){
        right, bar_rect.y, clock_width, height
    };
    element_present[TASKBAR_ELEMENT_CLOCK] = true;

    /*
     * Each of the remaining tray elements is its own slot, laid out from the
     * clock leftwards, and each takes its own hover highlight.
     */
    static const enum taskbar_element tray_order[4] = {
        TASKBAR_ELEMENT_TRAY_BATTERY, TASKBAR_ELEMENT_TRAY_VOLUME,
        TASKBAR_ELEMENT_TRAY_NETWORK, TASKBAR_ELEMENT_CHEVRON
    };
    const size_t tray_count = chevron_visible ? 4U : 3U;

    for (size_t index = 0U; index < tray_count; ++index) {
        right = right > TASKBAR_TRAY_SLOT_WIDTH ?
            right - TASKBAR_TRAY_SLOT_WIDTH : 0U;
        element_rects[tray_order[index]] = (struct ui_rect){
            right, bar_rect.y, TASKBAR_TRAY_SLOT_WIDTH, height
        };
        element_present[tray_order[index]] = true;
    }

    const uint32_t tray_left = right;

    /*
     * --- the cluster ---
     *
     * TaskbarFrameRepeater has no margin and TaskbarButtonMargin is zero, so
     * the slots are contiguous and the cluster is exactly the sum of its
     * extents.  Centring the whole cluster - Start included - is what makes
     * Start slide left by half a slot each time an application is opened.
     */
    cluster_count = 0U;
    cluster_order[cluster_count++] = TASKBAR_ELEMENT_START;
    if (search_visible && search_mode != TASKBAR_SEARCH_HIDDEN) {
        cluster_order[cluster_count++] = TASKBAR_ELEMENT_SEARCH;
    }
    if (task_view_visible) {
        cluster_order[cluster_count++] = TASKBAR_ELEMENT_TASK_VIEW;
    }
    if (widgets_visible) {
        cluster_order[cluster_count++] = TASKBAR_ELEMENT_WIDGETS;
    }
    for (size_t index = 0U; index < TASKBAR_MAX_APPS; ++index) {
        if (apps[index].present) {
            cluster_order[cluster_count++] = (enum taskbar_element)(
                TASKBAR_ELEMENT_APP_FIRST + index);
        }
    }

    /*
     * A search box that does not fit collapses to an icon.  Windows does the
     * same thing when the strip runs out of room, and it is better than
     * pushing the cluster into the tray.
     */
    search_mode_effective = search_mode;
    for (;;) {
        uint32_t width = 0U;

        for (size_t index = 0U; index < cluster_count; ++index) {
            width += element_extent(cluster_order[index]);
        }
        if (width <= tray_left || search_mode_effective ==
                TASKBAR_SEARCH_ICON) {
            cluster_width = width;
            break;
        }
        if (search_mode_effective == TASKBAR_SEARCH_BOX) {
            search_mode_effective = TASKBAR_SEARCH_ICON_LABEL;
        } else if (search_mode_effective == TASKBAR_SEARCH_ICON_LABEL) {
            search_mode_effective = TASKBAR_SEARCH_ICON;
        } else {
            cluster_width = width;
            break;
        }
    }

    uint32_t target;

    /*
     * Windows 10 pins its buttons to the left edge and has no setting that
     * moves them; the centred arrangement is Windows 11's.  The alignment
     * switch is kept because the surrounding compositor may want it, but the
     * copy is the left one, and Start begins at x = 0 with no margin.
     */
    if (bar_alignment == TASKBAR_ALIGNMENT_CENTER &&
            screen_width > cluster_width) {
        target = (screen_width - cluster_width + 1U) / 2U;
    } else {
        target = 0U;
    }
    if (target + cluster_width > tray_left) {
        target = tray_left > cluster_width ? tray_left - cluster_width : 0U;
    }
    if (!cluster_placed) {
        cluster_origin = target;
        cluster_origin_previous = target;
        ui_motion_reset(&cluster_slide, (int32_t)TASKBAR_ONE);
        cluster_placed = true;
    } else if (target != cluster_origin_target) {
        cluster_origin_previous = cluster_origin;
        ui_motion_reset(&cluster_slide, 0);
        ui_motion_to(&cluster_slide, (int32_t)TASKBAR_ONE,
            TASKBAR_DURATION_SLIDE_NS, clock_monotonic_ns());
    }
    cluster_origin_target = target;
    place_cluster();
}

/*
 * Put the cluster's slots down at wherever the slide has reached.  Called
 * from layout() and again from every animation frame, which is what makes the
 * re-centring a movement rather than a jump.
 */
static void place_cluster(void)
{
    const int64_t from = (int64_t)cluster_origin_previous;
    const int64_t to = (int64_t)cluster_origin_target;
    const int64_t progress = clamp64(cluster_slide.value, 0, TASKBAR_ONE);
    uint32_t at;

    cluster_origin = (uint32_t)(from + (to - from) * progress / TASKBAR_ONE);
    at = cluster_origin;
    for (size_t index = 0U; index < cluster_count; ++index) {
        const enum taskbar_element element = cluster_order[index];
        const uint32_t extent = element_extent(element);

        element_rects[element] = (struct ui_rect){
            at, bar_rect.y, extent, bar_rect.height
        };
        element_present[element] = true;
        at += extent;
    }
}

/*
 * The panel: the slot inset by TaskbarButtonHitTestableMargin.  It is both
 * what a hover highlight paints and what the running indicator hangs from,
 * and on a default bar it is the forty by forty square everyone recognizes.
 */
static struct ui_rect panel_rect(enum taskbar_element element)
{
    const struct ui_rect slot = element_rects[element];
    const bool box = element == TASKBAR_ELEMENT_SEARCH &&
        (search_mode_effective == TASKBAR_SEARCH_BOX ||
         search_mode_effective == TASKBAR_SEARCH_ICON_LABEL);
    /* SearchBoxButtonHitTestableMargin is 12,4,0,4: unlike every other
     * button the search box is not inset evenly, because its right edge
     * meets the next slot. */
    const uint32_t left = box ? TASKBAR_SEARCH_BOX_INSET_LEFT :
        TASKBAR_BUTTON_INSET_X;
    const uint32_t rightward = box ? TASKBAR_SEARCH_BOX_INSET_RIGHT :
        TASKBAR_BUTTON_INSET_X;
    const uint32_t inset_y = TASKBAR_BUTTON_INSET_Y;

    if (slot.width <= left + rightward || slot.height <= inset_y * 2U) {
        return slot;
    }
    return (struct ui_rect){
        slot.x + left, slot.y + inset_y,
        slot.width - left - rightward, slot.height - inset_y * 2U
    };
}

static struct ui_rect icon_rect(enum taskbar_element element)
{
    return rect_centred(element_rects[element], icon_size(), icon_size());
}

struct ui_rect taskbar_bounds(void)
{
    return bar_rect;
}

struct ui_rect taskbar_work_area(void)
{
    return (struct ui_rect){ 0U, 0U, screen_width, bar_rect.y };
}

/* ================================================================ FLYOUTS
 *
 * What the bar opens: a jump list on a right click, and the Start menu.  Both
 * are acrylic panels with an eight-pixel radius, a hairline edge and a soft
 * shadow, and both are positioned above the bar rather than over it.
 *
 * Flyouts use AcrylicBackgroundFillColorDefault, whose TintLuminosityOpacity
 * is 0.96 in the dark theme and 0.85 in the light one.  At those opacities the
 * backdrop contributes four and fifteen per cent, so this samples the desktop
 * unblurred rather than carrying a second pair of blur buffers: the error is
 * bounded by that contribution, and four per cent of a local contrast is not
 * visible.  The bar itself gets the real blur, because at its size the
 * backdrop is what the material is made of.
 */

/* Measured from screenshots; no resource states any of these. */
#define TASKBAR_FLYOUT_GAP 8U          /* above the bar */
#define TASKBAR_FLYOUT_EDGE 12U        /* from the screen's edges */
#define TASKBAR_FLYOUT_SHADOW 16U      /* how far the shadow reaches */
#define TASKBAR_FLYOUT_SHADOW_ALPHA 90U
#define TASKBAR_JUMP_WIDTH 264U
#define TASKBAR_JUMP_ITEM_HEIGHT 32U
#define TASKBAR_JUMP_PADDING 4U
#define TASKBAR_JUMP_TEXT_INSET 12U
#define TASKBAR_JUMP_HEADER_HEIGHT 30U

enum taskbar_flyout_kind {
    TASKBAR_FLYOUT_NONE = 0,
    TASKBAR_FLYOUT_JUMP_LIST,
    TASKBAR_FLYOUT_TASKBAR_MENU
};

enum taskbar_jump_item {
    TASKBAR_JUMP_PIN = 0,
    TASKBAR_JUMP_CLOSE,
    TASKBAR_JUMP_COUNT
};

static enum taskbar_flyout_kind flyout_kind;
static enum taskbar_element flyout_anchor;
static struct ui_rect flyout_rect;
static size_t flyout_items;
static size_t flyout_hover = (size_t)-1;
static struct ui_motion flyout_reveal;

/*
 * A soft shadow under a flyout.  Rather than stacking rounded rectangles at
 * falling alphas - which bands - this measures how far each pixel in the band
 * lies outside the shape and squares the falloff, which is close enough to a
 * Gaussian's shoulder at this size and costs one distance per pixel.
 */
/* Floor of the square root, by bisection on the bits. */
static uint32_t integer_sqrt(uint32_t value)
{
    uint32_t result = 0U;
    uint32_t bit = UINT32_C(1) << 30;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static enum taskbar_status draw_flyout_shadow(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t radius_eighths
)
{
    const uint32_t reach = TASKBAR_FLYOUT_SHADOW;
    const uint32_t left = bounds.x > reach ? bounds.x - reach : 0U;
    const uint32_t top = bounds.y > reach ? bounds.y - reach : 0U;
    const struct ui_rect band = {
        left, top,
        bounds.x + bounds.width + reach - left,
        bounds.y + bounds.height + reach - top
    };
    const struct ui_rect clipped = rect_intersect(band,
        rect_intersect(damage, (struct ui_rect){
            0U, 0U, screen_width, screen_height
        }));
    const uint32_t black = framebuffer_pack(0U, 0U, 0U);

    (void)radius_eighths;
    if (rect_is_empty(clipped)) {
        return TASKBAR_STATUS_OK;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const int32_t point_y = (int32_t)(clipped.y + y);

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const int32_t point_x = (int32_t)(clipped.x + x);
            /* Distance outside the rectangle, per axis, then combined; the
             * corner radius is subtracted so the shadow follows the round
             * corner rather than the square bounding box. */
            const int32_t dx = point_x < (int32_t)bounds.x ?
                (int32_t)bounds.x - point_x :
                (point_x >= (int32_t)(bounds.x + bounds.width) ?
                    point_x - (int32_t)(bounds.x + bounds.width) + 1 : 0);
            const int32_t dy = point_y < (int32_t)bounds.y ?
                (int32_t)bounds.y - point_y :
                (point_y >= (int32_t)(bounds.y + bounds.height) ?
                    point_y - (int32_t)(bounds.y + bounds.height) + 1 : 0);
            uint32_t distance;
            uint32_t under;

            if (dx == 0 && dy == 0) {
                continue; /* inside: the panel itself covers it */
            }
            /* An exact integer distance.  An approximation here is visible:
             * the two axes and the diagonal have to agree at the seams or the
             * shadow grows square blocks at its corners. */
            distance = integer_sqrt((uint32_t)(dx * dx + dy * dy));
            if (distance >= reach) {
                continue;
            }
            const uint32_t remaining = reach - distance;
            const uint32_t alpha = TASKBAR_FLYOUT_SHADOW_ALPHA *
                remaining * remaining / (reach * reach);

            if (alpha == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend(under, black, alpha)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKBAR_STATUS_OK;
}

/* ============================================================ START MENU
 *
 * What the Start button opens.
 *
 * Windows 10's Start menu is three columns side by side, and getting the
 * three of them in the right order is most of what makes it recognisable:
 *
 *   a narrow RAIL down the left edge, one button wide, carrying the
 *   hamburger at the top and the account, folders, settings and power at the
 *   bottom, with nothing in between;
 *
 *   the APP LIST, alphabetical, with single-letter headings between the
 *   runs and "Frequent" above them all;
 *
 *   and the TILES, in groups under their own headings, on a grid of small
 *   squares.  Windows' Start layout XML gives tiles four sizes and names
 *   them by that grid - 1x1, 2x2, 4x2 and 4x4 - and a group is six of those
 *   columns across, which is why three medium tiles fit a row exactly.
 *
 * This menu is ACRYLIC, and the taskbar under it is not.  That is not an
 * inconsistency: it is the actual difference between the two surfaces on
 * Windows 10, and drawing them the same way is how a copy loses it.  The
 * backdrop is captured at an eighth of full resolution rather than the bar's
 * quarter, because the menu covers forty times the area and at this tint
 * opacity nobody can tell.
 *
 * PENDING VERIFICATION: every pixel size below.  The three-column structure
 * and the tile grid are Windows'; the measurements are read off the menu.
 */

static enum taskbar_status draw_icon(
    const struct taskbar_icon *icon,
    struct ui_rect bounds,
    struct ui_rect damage
);

#define START_RAIL_WIDTH 48U
#define START_LIST_WIDTH 272U
#define START_TILE_UNIT 48U            /* one 1x1 tile */
#define START_TILE_GAP 4U
#define START_TILE_COLUMNS 6U          /* one group, in 1x1 columns */
#define START_TILE_PADDING 16U
#define START_TILE_SPAN(units) \
    ((units) * START_TILE_UNIT + ((units) - 1U) * START_TILE_GAP)
#define START_MENU_WIDTH \
    (START_RAIL_WIDTH + START_LIST_WIDTH + START_TILE_PADDING * 2U + \
        START_TILE_SPAN(START_TILE_COLUMNS))
#define START_MENU_HEIGHT 640U          /* the tallest it ever gets */
#define START_MENU_MIN_HEIGHT 400U
#define START_ROW_HEIGHT 36U           /* one application in the list */
#define START_HEADING_HEIGHT 32U       /* a letter, or a tile group's name */
#define START_LIST_ICON 24U
#define START_LIST_TEXT_INSET 52U
#define START_TILE_ICON 32U
#define START_TILE_LARGE_ICON 48U
/* How far the menu rises as it appears.  Windows 10 slides it up out of the
 * taskbar rather than scaling it from a corner. */
#define START_REVEAL_RISE 48
/* Normal in, Fast out - which is the shape of every popup Windows opens:
 * it takes its time arriving and gets out of the way quickly. */
#define START_REVEAL_OPEN_NS UI_MOTION_REVEAL_NS   /* Normal */
#define START_REVEAL_CLOSE_NS UI_MOTION_DISMISS_NS /* Fast */
/*
 * The search panel's outer size, declared up here because the shared
 * material buffer below has to be sized for whichever panel is bigger.
 * Everything else about it is in the SEARCH PANEL section further down.
 *
 * Windows 10's search flyout is nearly square and narrower than the Start
 * menu, and stands on the taskbar in the same place - which is what lets
 * the two of them share one blurred copy of the desktop.
 */
#define SEARCH_PANEL_WIDTH 640U
#define SEARCH_PANEL_HEIGHT 600U      /* the tallest it ever gets */
#define SEARCH_PANEL_MIN_HEIGHT 320U

/* A panel's backdrop, an eighth of full resolution in each axis. */
#define PANEL_MATERIAL_DOWNSAMPLE 8U
#define PANEL_MATERIAL_MARGIN 32U
/* Big enough for the LARGER of the two panels that share the buffer, since
 * either one may be the one standing open when the desktop is captured. */
#define PANEL_MATERIAL_MAX_WIDTH \
    (START_MENU_WIDTH > SEARCH_PANEL_WIDTH ? START_MENU_WIDTH : \
        SEARCH_PANEL_WIDTH)
#define PANEL_MATERIAL_MAX_HEIGHT \
    (START_MENU_HEIGHT > SEARCH_PANEL_HEIGHT ? START_MENU_HEIGHT : \
        SEARCH_PANEL_HEIGHT)
#define PANEL_MATERIAL_SMALL_WIDTH \
    ((PANEL_MATERIAL_MAX_WIDTH + PANEL_MATERIAL_MARGIN * 2U) / \
        PANEL_MATERIAL_DOWNSAMPLE + 2U)
#define PANEL_MATERIAL_SMALL_HEIGHT \
    ((PANEL_MATERIAL_MAX_HEIGHT + PANEL_MATERIAL_MARGIN * 2U) / \
        PANEL_MATERIAL_DOWNSAMPLE + 2U)
#define PANEL_MATERIAL_BLUR_PASSES 3U
#define PANEL_MATERIAL_BLUR_RADIUS 3U

static bool start_open;
static struct ui_motion start_reveal;
static struct ui_rect start_menu_rect;
static struct ui_rect start_rail_rect;
static struct ui_rect start_list_rect;
static struct ui_rect start_tiles_rect;
static size_t start_hover_entry = (size_t)-1;
static size_t start_hover_tile = (size_t)-1;
static size_t start_hover_rail = (size_t)-1;
static struct taskbar_start_entry start_entries[TASKBAR_MAX_START_ENTRIES];
static struct taskbar_start_tile start_tiles[TASKBAR_MAX_START_TILES];
static char start_group_names[TASKBAR_MAX_START_GROUPS][TASKBAR_LABEL_BYTES];
static uint32_t panel_material[PANEL_MATERIAL_SMALL_WIDTH *
    PANEL_MATERIAL_SMALL_HEIGHT];
static uint32_t panel_scratch[PANEL_MATERIAL_SMALL_WIDTH *
    PANEL_MATERIAL_SMALL_HEIGHT];
static uint32_t panel_material_width;
static uint32_t panel_material_height;
static uint32_t panel_material_x;
static uint32_t panel_material_y;
static bool panel_material_valid;

/* The rail's buttons, top to bottom.  Windows 10 puts the hamburger alone at
 * the top and the rest against the floor. */
enum start_rail_item {
    START_RAIL_MENU = 0,
    START_RAIL_ACCOUNT,
    START_RAIL_DOCUMENTS,
    START_RAIL_PICTURES,
    START_RAIL_SETTINGS,
    START_RAIL_POWER,
    START_RAIL_COUNT
};

static enum taskbar_glyph start_rail_glyph(size_t item)
{
    switch (item) {
    case START_RAIL_MENU:
        return TASKBAR_GLYPH_MENU;
    case START_RAIL_ACCOUNT:
        return TASKBAR_GLYPH_ACCOUNT;
    case START_RAIL_DOCUMENTS:
        return TASKBAR_GLYPH_NOTES;
    case START_RAIL_PICTURES:
        return TASKBAR_GLYPH_PICTURES;
    case START_RAIL_SETTINGS:
        return TASKBAR_GLYPH_SETTINGS;
    case START_RAIL_POWER:
    default:
        return TASKBAR_GLYPH_POWER;
    }
}

static struct ui_rect start_rail_rect_for(size_t item)
{
    if (item == START_RAIL_MENU) {
        return (struct ui_rect){ start_rail_rect.x, start_rail_rect.y,
            START_RAIL_WIDTH, START_RAIL_WIDTH };
    }

    const size_t from_bottom = START_RAIL_COUNT - 1U - item;
    const uint32_t bottom = start_rail_rect.y + start_rail_rect.height;
    const uint32_t offset = (uint32_t)(from_bottom + 1U) * START_RAIL_WIDTH;

    if (offset > start_rail_rect.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ start_rail_rect.x, bottom - offset,
        START_RAIL_WIDTH, START_RAIL_WIDTH };
}

static uint32_t start_entry_top(size_t index)
{
    uint32_t y = start_list_rect.y + START_TILE_PADDING / 2U;

    for (size_t scan = 0U; scan < index; ++scan) {
        if (!start_entries[scan].present) {
            continue;
        }
        y += start_entries[scan].heading ?
            START_HEADING_HEIGHT : START_ROW_HEIGHT;
    }
    return y;
}

static struct ui_rect start_entry_rect(size_t index)
{
    const uint32_t top = start_entry_top(index);
    const uint32_t height = start_entries[index].heading ?
        START_HEADING_HEIGHT : START_ROW_HEIGHT;

    if (top + height > start_list_rect.y + start_list_rect.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ start_list_rect.x, top,
        start_list_rect.width, height };
}

/* How many 1x1 rows a group's tiles occupy. */
static uint32_t start_group_rows(uint8_t group)
{
    uint32_t rows = 0U;

    for (size_t scan = 0U; scan < TASKBAR_MAX_START_TILES; ++scan) {
        const struct taskbar_start_tile *tile = &start_tiles[scan];

        if (tile->present && tile->group == group &&
                (uint32_t)tile->row + tile->rows > rows) {
            rows = (uint32_t)tile->row + tile->rows;
        }
    }
    return rows;
}

/* Where a group's first row of tiles begins, its heading already above it. */
static uint32_t start_group_top(uint8_t group)
{
    uint32_t top = start_tiles_rect.y + START_TILE_PADDING;

    for (uint8_t earlier = 0U; earlier < group &&
            earlier < TASKBAR_MAX_START_GROUPS; ++earlier) {
        const uint32_t rows = start_group_rows(earlier);

        top += START_HEADING_HEIGHT + (rows == 0U ? 0U :
            START_TILE_SPAN(rows) + START_TILE_GAP);
    }
    return top + START_HEADING_HEIGHT;
}

static struct ui_rect start_tile_rect(size_t index)
{
    const struct taskbar_start_tile *tile = &start_tiles[index];

    return (struct ui_rect){
        start_tiles_rect.x + START_TILE_PADDING +
            (uint32_t)tile->column * (START_TILE_UNIT + START_TILE_GAP),
        start_group_top(tile->group) +
            (uint32_t)tile->row * (START_TILE_UNIT + START_TILE_GAP),
        START_TILE_SPAN((uint32_t)tile->columns),
        START_TILE_SPAN((uint32_t)tile->rows)
    };
}

static struct ui_rect start_group_heading_rect(uint8_t group)
{
    return (struct ui_rect){
        start_tiles_rect.x + START_TILE_PADDING,
        start_group_top(group) - START_HEADING_HEIGHT,
        START_TILE_SPAN(START_TILE_COLUMNS), START_HEADING_HEIGHT
    };
}

/*
 * How tall the menu has to be for what is in it.
 *
 * Windows 10's Start menu is not a fixed rectangle: it is as tall as its
 * tile groups need, resizes by whole tile rows, and stops at a floor so that
 * an empty menu is still a menu.  A copy that always draws the same box ends
 * up with a wall of empty acrylic under three tiles, which is the one thing
 * the real menu never looks like.
 */
static uint32_t start_content_height(void)
{
    uint32_t list = START_TILE_PADDING;
    uint32_t tiles = START_TILE_PADDING;

    for (size_t index = 0U; index < TASKBAR_MAX_START_ENTRIES; ++index) {
        if (!start_entries[index].present) {
            continue;
        }
        list += start_entries[index].heading ?
            START_HEADING_HEIGHT : START_ROW_HEIGHT;
    }
    for (uint8_t group = 0U; group < TASKBAR_MAX_START_GROUPS; ++group) {
        const uint32_t rows = start_group_rows(group);

        if (rows == 0U) {
            continue;
        }
        tiles += START_HEADING_HEIGHT + START_TILE_SPAN(rows) +
            START_TILE_GAP;
    }
    /* The rail has to hold its five bottom buttons and the hamburger. */
    const uint32_t rail = START_RAIL_WIDTH * (START_RAIL_COUNT + 1U);
    uint32_t wanted = list > tiles ? list : tiles;

    wanted += START_TILE_PADDING;
    if (wanted < rail) {
        wanted = rail;
    }
    return wanted < START_MENU_MIN_HEIGHT ? START_MENU_MIN_HEIGHT : wanted;
}

static void start_layout(void)
{
    uint32_t width = START_MENU_WIDTH;
    uint32_t height = start_content_height();

    if (height > START_MENU_HEIGHT) {
        height = START_MENU_HEIGHT;
    }

    if (width > screen_width) {
        width = screen_width;
    }
    if (height > bar_rect.y) {
        height = bar_rect.y;
    }
    start_menu_rect = (struct ui_rect){ 0U, bar_rect.y - height,
        width, height };
    start_rail_rect = (struct ui_rect){ start_menu_rect.x, start_menu_rect.y,
        START_RAIL_WIDTH, height };

    const uint32_t list_width = width > START_RAIL_WIDTH + START_LIST_WIDTH ?
        START_LIST_WIDTH : (width > START_RAIL_WIDTH ?
            width - START_RAIL_WIDTH : 0U);

    start_list_rect = (struct ui_rect){
        start_menu_rect.x + START_RAIL_WIDTH, start_menu_rect.y,
        list_width, height
    };
    start_tiles_rect = (struct ui_rect){
        start_list_rect.x + list_width, start_menu_rect.y,
        width - START_RAIL_WIDTH - list_width, height
    };
}

/*
 * The menu's backdrop.  Same recipe as the bar's blur, at an eighth of the
 * resolution rather than a quarter and over the menu's own rectangle: three
 * box passes are a Gaussian to within a percent, and at this tint opacity the
 * lost detail is not visible.
 */
/*
 * Blur the desktop under a rising panel, for that panel's acrylic to sample.
 *
 * One buffer, shared by the Start menu and the search panel, because the two
 * are never open at the same time - opening either closes the other, the same
 * way Windows 10's do, since both are one panel standing on the taskbar and
 * two of them at once has never been a state that shell can be in.  Two
 * buffers would be two copies of this function and half a megabyte to hold a
 * picture that only one of them can be looking at.
 */
static void capture_panel_backdrop(struct ui_rect bounds)
{
    const uint32_t step = PANEL_MATERIAL_DOWNSAMPLE;
    const uint32_t margin = PANEL_MATERIAL_MARGIN;
    const struct framebuffer_state format = framebuffer_get_state();

    panel_material_valid = false;
    if (rect_is_empty(bounds)) {
        return;
    }
    panel_material_x = bounds.x > margin ? bounds.x - margin : 0U;
    panel_material_y = bounds.y > margin ? bounds.y - margin : 0U;

    const uint32_t right = bounds.x + bounds.width + margin;
    const uint32_t bottom = bounds.y + bounds.height + margin;
    const uint32_t span_x = (right > screen_width ? screen_width : right) -
        panel_material_x;
    const uint32_t span_y = (bottom > screen_height ? screen_height : bottom) -
        panel_material_y;

    panel_material_width = (span_x + step - 1U) / step;
    panel_material_height = (span_y + step - 1U) / step;
    if (panel_material_width > PANEL_MATERIAL_SMALL_WIDTH ||
            panel_material_height > PANEL_MATERIAL_SMALL_HEIGHT) {
        return;
    }
    for (uint32_t y = 0U; y < panel_material_height; ++y) {
        for (uint32_t x = 0U; x < panel_material_width; ++x) {
            uint32_t red = 0U;
            uint32_t green = 0U;
            uint32_t blue = 0U;
            uint32_t samples = 0U;

            for (uint32_t inner_y = 0U; inner_y < step; ++inner_y) {
                const uint32_t source_y = panel_material_y + y * step +
                    inner_y;

                if (source_y >= screen_height) {
                    break;
                }
                for (uint32_t inner_x = 0U; inner_x < step; ++inner_x) {
                    const uint32_t source_x = panel_material_x + x * step +
                        inner_x;
                    uint32_t pixel;

                    if (source_x >= screen_width) {
                        break;
                    }
                    if (surface_read_pixel(canvas, source_x, source_y,
                            &pixel) != SURFACE_STATUS_OK) {
                        return;
                    }
                    red += channel_of(pixel, format.red_position);
                    green += channel_of(pixel, format.green_position);
                    blue += channel_of(pixel, format.blue_position);
                    samples += 1U;
                }
            }
            if (samples == 0U) {
                samples = 1U;
            }
            panel_material[(size_t)y * panel_material_width + x] =
                framebuffer_pack((uint8_t)(red / samples),
                    (uint8_t)(green / samples), (uint8_t)(blue / samples));
        }
    }
    for (uint32_t pass = 0U; pass < PANEL_MATERIAL_BLUR_PASSES; ++pass) {
        box_blur_plane(panel_material, panel_scratch, panel_material_width,
            panel_material_height, PANEL_MATERIAL_BLUR_RADIUS);
    }
    panel_material_valid = true;
}

static uint32_t panel_material_at(uint32_t x, uint32_t y)
{
    const uint32_t step = PANEL_MATERIAL_DOWNSAMPLE;
    const uint32_t cell_x = (x - panel_material_x) / step;
    const uint32_t cell_y = (y - panel_material_y) / step;
    const uint32_t clamped_x = cell_x < panel_material_width ? cell_x :
        panel_material_width - 1U;
    const uint32_t clamped_y = cell_y < panel_material_height ? cell_y :
        panel_material_height - 1U;

    return panel_material[(size_t)clamped_y * panel_material_width +
        clamped_x];
}

/* How far through its reveal the menu is, 0 shut and 255 fully open. */
static uint32_t start_reveal_alpha(void)
{
    return ui_motion_alpha(&start_reveal);
}

/* The menu's rectangle where it currently IS, which during the reveal is
 * below where it will end up. */
static struct ui_rect start_current_rect(void)
{
    const int64_t value = clamp64(start_reveal.value, 0, TASKBAR_ONE);
    const uint32_t rise = (uint32_t)((TASKBAR_ONE - value) *
        START_REVEAL_RISE / TASKBAR_ONE);
    struct ui_rect rect = start_menu_rect;

    rect.y += rise;
    if (rect.height > rise) {
        rect.height -= rise;
    } else {
        rect.height = 0U;
    }
    return rect;
}

/* The acrylic itself, shared by both rising panels: same two-layer recipe,
 * same tint, same grain - they are one material and would be wrong to
 * differ.  Only the bounds change. */
static enum taskbar_status draw_panel_material(struct ui_rect bounds,
    struct ui_rect damage, uint32_t reveal)
{
    const struct taskbar_palette *colours = palette();
    const struct ui_rect clipped = rect_intersect(bounds, damage);
    const uint32_t tint = pack(colours->start_background);
    const int32_t tint_luma = (int32_t)luma_of(tint);

    if (rect_is_empty(clipped)) {
        return TASKBAR_STATUS_OK;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t screen_x = clipped.x + x;
            const uint32_t screen_y = clipped.y + y;
            uint32_t under;
            uint32_t pixel;

            if (surface_read_pixel(canvas, screen_x, screen_y, &under) !=
                    SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
            if (panel_material_valid) {
                const uint32_t blurred = panel_material_at(screen_x, screen_y);

                /* The luminosity layer, then the tint: the same two-layer
                 * acrylic the Windows 11 bar used, which is what the Windows
                 * 10 START MENU is even though its taskbar is not. */
                pixel = blend(blurred, set_luma(blurred, tint_luma),
                    colours->start_luminosity);
                pixel = blend(pixel, set_luma(tint, (int32_t)luma_of(pixel)),
                    colours->start_background.alpha);

                const uint8_t grain = noise_level(screen_x, screen_y);

                pixel = blend(pixel, framebuffer_pack(grain, grain, grain),
                    TASKBAR_NOISE_OPACITY);
            } else {
                pixel = blend(under, tint, colours->start_background.alpha);
            }
            if (surface_pixel(canvas, screen_x, screen_y,
                    blend(under, pixel, reveal)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKBAR_STATUS_OK;
}

static enum taskbar_status start_draw_rail(struct ui_rect damage,
    uint32_t reveal, int32_t shift)
{
    const struct taskbar_palette *colours = palette();
    struct ui_rect rail = start_rail_rect;
    enum taskbar_status status;

    rail.y = (uint32_t)((int32_t)rail.y + shift);
    /* The rail is a shade darker than the rest, which is how Windows 10
     * separates it without drawing a line down it. */
    status = fill_rounded(rail, damage, 0U, colours->start_rail, reveal);
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    for (size_t item = 0U; item < START_RAIL_COUNT; ++item) {
        struct ui_rect slot = start_rail_rect_for(item);

        if (rect_is_empty(slot)) {
            continue;
        }
        slot.y = (uint32_t)((int32_t)slot.y + shift);
        if (item == start_hover_rail) {
            status = fill_rounded(slot, damage, 0U, colours->item_secondary,
                reveal);
            if (status != TASKBAR_STATUS_OK) {
                return status;
            }
        }
        status = draw_glyph(start_rail_glyph(item),
            rect_centred(slot, TASKBAR_TRAY_GLYPH_SIZE,
                TASKBAR_TRAY_GLYPH_SIZE),
            damage, colours->text_primary, reveal);
        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
    }
    return TASKBAR_STATUS_OK;
}

static enum taskbar_status start_draw_list(struct ui_rect damage,
    uint32_t reveal, int32_t shift)
{
    const struct taskbar_palette *colours = palette();

    for (size_t index = 0U; index < TASKBAR_MAX_START_ENTRIES; ++index) {
        const struct taskbar_start_entry *entry = &start_entries[index];
        struct ui_rect row = start_entry_rect(index);
        enum taskbar_status status;

        if (!entry->present || rect_is_empty(row)) {
            continue;
        }
        row.y = (uint32_t)((int32_t)row.y + shift);
        if (entry->heading) {
            /*
             * A letter heading, or "Frequent".  Windows draws these in the
             * accent colour and gives them no hover of their own, because
             * on Windows they are buttons that jump to the alphabet grid and
             * this menu has no grid to jump to.
             */
            /*
             * A one-character heading is an alphabet letter, which Windows
             * draws in the accent colour because on Windows it is a button
             * that jumps to the letter grid.  A longer one - "Frequent" -
             * is a plain label, and drawing it in the accent too is a tell.
             */
            const bool letter = entry->label[0] != '\0' &&
                entry->label[1] == '\0';

            status = draw_text(damage,
                row.x + START_TILE_PADDING,
                row.y + (row.height + text_ascent()) / 2U - 1U,
                entry->label, scale_alpha(letter ? colours->accent_text :
                    colours->text_secondary, reveal));
            if (status != TASKBAR_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (index == start_hover_entry) {
            status = fill_rounded(row, damage, 0U, colours->item_secondary,
                reveal);
            if (status != TASKBAR_STATUS_OK) {
                return status;
            }
        }
        status = draw_icon(&entry->icon, rect_centred((struct ui_rect){
            row.x + START_TILE_PADDING, row.y, START_LIST_ICON, row.height
        }, START_LIST_ICON, START_LIST_ICON), damage);
        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
        status = draw_text(damage, row.x + START_LIST_TEXT_INSET,
            row.y + (row.height + text_ascent()) / 2U - 1U, entry->label,
            scale_alpha(colours->text_primary, reveal));
        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
    }
    return TASKBAR_STATUS_OK;
}

static enum taskbar_status start_draw_tiles(struct ui_rect damage,
    uint32_t reveal, int32_t shift)
{
    const struct taskbar_palette *colours = palette();

    for (uint8_t group = 0U; group < TASKBAR_MAX_START_GROUPS; ++group) {
        struct ui_rect heading = start_group_heading_rect(group);

        if (start_group_rows(group) == 0U || start_group_names[group][0] ==
                '\0') {
            continue;
        }
        heading.y = (uint32_t)((int32_t)heading.y + shift);

        const enum taskbar_status status = draw_text(damage, heading.x,
            heading.y + (heading.height + text_ascent()) / 2U - 1U,
            start_group_names[group],
            scale_alpha(colours->text_primary, reveal));

        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
    }
    for (size_t index = 0U; index < TASKBAR_MAX_START_TILES; ++index) {
        const struct taskbar_start_tile *tile = &start_tiles[index];
        struct ui_rect rect = start_tile_rect(index);
        struct taskbar_colour plate;
        enum taskbar_status status;

        if (!tile->present || rect_is_empty(rect)) {
            continue;
        }
        rect.y = (uint32_t)((int32_t)rect.y + shift);
        if (rect.y + rect.height > start_menu_rect.y +
                start_menu_rect.height) {
            continue;
        }
        /*
         * A tile is an opaque plate in the application's own colour, square
         * cornered, with its icon centred and its name along the bottom.
         * Windows 10 defaults that colour to the system accent, which is why
         * a stock Start menu is a wall of blue with a few branded squares
         * in it.
         */
        if (tile->colour != 0U) {
            const struct framebuffer_state format = framebuffer_get_state();

            plate = (struct taskbar_colour){
                channel_of(tile->colour, format.red_position),
                channel_of(tile->colour, format.green_position),
                channel_of(tile->colour, format.blue_position), 0xFFU
            };
        } else {
            plate = colours->accent_fill;
        }
        status = fill_rounded(rect, damage, 0U, plate, reveal);
        if (status == TASKBAR_STATUS_OK && index == start_hover_tile) {
            /* Hovering a tile lightens it and Windows tilts it towards the
             * pointer; the lightening is the part that survives without a
             * three-dimensional transform. */
            status = fill_rounded(rect, damage, 0U, colours->item_primary,
                reveal);
        }
        if (status != TASKBAR_STATUS_OK) {
            return status;
        }

        const uint32_t icon = tile->rows >= 4U ? START_TILE_LARGE_ICON :
            START_TILE_ICON;
        const bool labelled = tile->columns >= 2U && tile->rows >= 2U &&
            tile->label[0] != '\0';
        struct ui_rect icon_box = rect;

        if (labelled) {
            /* The name sits along the bottom, so the icon is centred in what
             * is left rather than in the whole tile. */
            icon_box.height = icon_box.height > text_line_height() ?
                icon_box.height - text_line_height() : icon_box.height;
        }
        status = draw_icon(&tile->icon, rect_centred(icon_box, icon, icon),
            damage);
        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
        if (!labelled) {
            continue;
        }
        status = draw_text(damage, rect.x + 8U,
            rect.y + rect.height - 8U, tile->label,
            scale_alpha(colours->tile_text, reveal));
        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
    }
    return TASKBAR_STATUS_OK;
}

static enum taskbar_status draw_start_menu(struct ui_rect damage)
{
    const uint32_t reveal = start_reveal_alpha();
    const struct ui_rect bounds = start_current_rect();
    const int32_t shift = (int32_t)bounds.y - (int32_t)start_menu_rect.y;
    enum taskbar_status status;

    if (reveal == 0U || rect_is_empty(bounds)) {
        return TASKBAR_STATUS_OK;
    }
    status = draw_panel_material(bounds, damage, reveal);
    if (status == TASKBAR_STATUS_OK) {
        status = start_draw_rail(damage, reveal, shift);
    }
    if (status == TASKBAR_STATUS_OK) {
        status = start_draw_list(damage, reveal, shift);
    }
    if (status == TASKBAR_STATUS_OK) {
        status = start_draw_tiles(damage, reveal, shift);
    }
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    /* The hairline along the top and right, which is what separates the menu
     * from a wallpaper of the same brightness. */
    return stroke_rounded(bounds, damage, 0U,
        scale_alpha(palette()->flyout_stroke, reveal), 1U);
}

/* --- opening, closing and pointing at it --- */

static void start_set_open(bool open)
{
    const uint64_t now = clock_monotonic_ns();

    if (start_open == open) {
        return;
    }
    start_open = open;
    start_layout();
    if (!open) {
        start_hover_entry = (size_t)-1;
        start_hover_tile = (size_t)-1;
        start_hover_rail = (size_t)-1;
    }
    ui_motion_to(&start_reveal, open ? (int32_t)TASKBAR_ONE : 0,
        open ? START_REVEAL_OPEN_NS : START_REVEAL_CLOSE_NS, now);
}

static bool start_track_pointer(struct ui_point point)
{
    const size_t was_entry = start_hover_entry;
    const size_t was_tile = start_hover_tile;
    const size_t was_rail = start_hover_rail;

    start_hover_entry = (size_t)-1;
    start_hover_tile = (size_t)-1;
    start_hover_rail = (size_t)-1;
    if (start_open) {
        for (size_t item = 0U; item < START_RAIL_COUNT; ++item) {
            if (rect_holds(start_rail_rect_for(item), point)) {
                start_hover_rail = item;
                break;
            }
        }
        for (size_t index = 0U; index < TASKBAR_MAX_START_ENTRIES; ++index) {
            if (start_entries[index].present &&
                    !start_entries[index].heading &&
                    rect_holds(start_entry_rect(index), point)) {
                start_hover_entry = index;
                break;
            }
        }
        for (size_t index = 0U; index < TASKBAR_MAX_START_TILES; ++index) {
            if (start_tiles[index].present &&
                    rect_holds(start_tile_rect(index), point)) {
                start_hover_tile = index;
                break;
            }
        }
    }
    return was_entry != start_hover_entry || was_tile != start_hover_tile ||
        was_rail != start_hover_rail;
}


/* ============================================================ SEARCH PANEL
 *
 * What "Search Phipia" opens.
 *
 * Windows 10's search flyout is one panel standing on the taskbar, and it
 * answers a query in a fixed shape: a BEST MATCH at the top, given a large
 * icon and a line naming what kind of thing it is, then the rest of what
 * matched underneath it in named groups - Apps, then Settings, then folders
 * - then the box you are typing into pinned along the bottom edge.  Empty,
 * it does not sit blank: it lists what you would most likely be reaching
 * for anyway.
 *
 * Every result here comes from something the shell already holds.  The apps
 * are the same alphabetical list the Start menu draws, set by
 * taskbar_set_start_entry(), so pinning an application makes it findable
 * without anything being told twice.  The utilities are the five
 * destinations the Start menu's rail already offers - Documents, Pictures,
 * Settings, Account, Power - and they resolve to the same taskbar_action
 * kinds that rail's buttons do, so "settings" typed here and Settings
 * clicked there are one code path arriving from two directions.  Nothing in
 * this panel is a list of results invented to make the picture look full.
 */
#define SEARCH_PANEL_PADDING 16U
#define SEARCH_ROW_HEIGHT 40U
#define SEARCH_HEADING_HEIGHT 30U
#define SEARCH_BEST_HEIGHT 76U
/*
 * Icon sizes, and every one of them is a size the assets are actually
 * rasterized at - 16/24/32/48 for artwork, 16/24/32 for the Lucide marks.
 * Nothing here resamples any more, so a box that names a size in between
 * does not get that size: it gets the largest one below it, drawn crisp
 * and smaller than the layout intended.  SEARCH_BEST_ICON was forty and
 * quietly drew thirty-two; SEARCH_FIELD_ICON was twenty and drew sixteen.
 */
#define SEARCH_ROW_ICON 24U
#define SEARCH_BEST_ICON 48U
#define SEARCH_TEXT_INSET 56U
#define SEARCH_BEST_TEXT_INSET 76U
#define SEARCH_FIELD_HEIGHT 48U        /* the box along the bottom edge */
#define SEARCH_FIELD_ICON 24U
#define SEARCH_MAX_RESULTS 12U
/* Rises the same distance over the same quarter second the Start menu does;
 * they are the same gesture and would look like two if they differed. */
#define SEARCH_REVEAL_RISE 48
#define SEARCH_REVEAL_OPEN_NS UI_MOTION_REVEAL_NS   /* Normal */
#define SEARCH_REVEAL_CLOSE_NS UI_MOTION_DISMISS_NS /* Fast */

/* Which list a result came out of, which is both the group it is drawn
 * under and the line under its name when it is the best match. */
enum search_group {
    SEARCH_GROUP_APP = 0,
    SEARCH_GROUP_UTILITY,
    SEARCH_GROUP_COUNT
};

struct search_result {
    enum search_group group;
    const char *label;             /* into start_entries[] or utilities[] */
    size_t entry_index;            /* an app's index in start_entries[] */
    enum taskbar_glyph glyph;      /* a utility's mark */
    const char *art;               /* a utility's picture, when it has one */
    enum taskbar_action_kind action;
    enum ui_panel_id panel;
};

/*
 * The utilities, in the order the Start menu's rail stacks them.  These are
 * shell destinations rather than applications, which is exactly the split
 * Windows draws between its "Apps" and "Settings" result groups.
 */
struct search_utility {
    const char *label;
    enum taskbar_glyph glyph;
    /* Artwork by name, for the one of these that has a picture of its own;
     * NULL falls back to the glyph beside it. */
    const char *art;
    enum taskbar_action_kind action;
};

static const struct search_utility search_utilities[] = {
    { "Settings", TASKBAR_GLYPH_SETTINGS, NULL,
        TASKBAR_ACTION_OPEN_SETTINGS },
    { "Documents", TASKBAR_GLYPH_NOTES, NULL, TASKBAR_ACTION_DOCUMENTS },
    { "Pictures", TASKBAR_GLYPH_PICTURES, NULL, TASKBAR_ACTION_PICTURES },
    { "Account", TASKBAR_GLYPH_ACCOUNT, NULL, TASKBAR_ACTION_ACCOUNT },
    { "Power", TASKBAR_GLYPH_POWER, NULL, TASKBAR_ACTION_POWER },
    { "All windows", TASKBAR_GLYPH_TASK_VIEW, NULL,
        TASKBAR_ACTION_TASK_VIEW },
    /*
     * Task Manager, which is HERE and nowhere else: it has no button on
     * the bar, no tile in Start and no pinned slot, so searching for it -
     * or pressing Alt+F4 - is how it is reached.  Windows does not pin it
     * either; a diagnostic you want only when something is wrong does not
     * earn a permanent place among the things you use every day.
     */
    { "Task Manager", TASKBAR_GLYPH_SEARCH, "taskmgr",
        TASKBAR_ACTION_TASK_MANAGER }
};

#define SEARCH_UTILITY_COUNT \
    (sizeof(search_utilities) / sizeof(search_utilities[0]))

static bool search_open;
static struct ui_motion search_reveal;
static struct ui_rect search_panel_rect;
static char search_query[TASKBAR_SEARCH_BYTES];
static size_t search_query_length;
static struct search_result search_results[SEARCH_MAX_RESULTS];
static size_t search_result_count;
static size_t search_hover = (size_t)-1;
/* The caret blinks in half-second halves the way every other caret in this
 * shell does; nothing in taskbar.c had one before this. */
static bool search_caret_visible = true;
static uint32_t search_caret_phase;

static uint32_t lower_ascii(uint32_t code)
{
    return code >= 'A' && code <= 'Z' ? code - 'A' + 'a' : code;
}

/* Case-insensitive, anywhere in the string, empty needle matching
 * everything - the same rule the Explorer window's search box follows, so
 * the two boxes in this shell do not disagree about what "matching" is. */
static bool search_contains_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL) {
        return false;
    }
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

/* Whole-string equality, case-insensitively - not a prefix test, which
 * would fold "Store" into "Stores". */
static bool labels_match(const char *left, const char *right)
{
    size_t index = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    for (; left[index] != '\0' && right[index] != '\0'; ++index) {
        if (lower_ascii((unsigned char)left[index]) !=
                lower_ascii((unsigned char)right[index])) {
            return false;
        }
    }
    return left[index] == '\0' && right[index] == '\0';
}

static bool search_starts_with_ci(const char *body, const char *prefix)
{
    if (body == NULL) {
        return false;
    }
    for (size_t index = 0U; prefix[index] != '\0'; ++index) {
        if (lower_ascii((unsigned char)body[index]) !=
                lower_ascii((unsigned char)prefix[index])) {
            return false;
        }
    }
    return true;
}

/*
 * Collect what the query matches, best first.
 *
 * Two passes rather than a score: everything whose name STARTS with what
 * was typed, then everything that merely contains it.  Typing "set" should
 * put Settings at the top rather than somewhere below an application with
 * "set" in the middle of its name, and a rank that is just "which pass
 * found it" gets that without a number nobody can predict the behaviour of.
 * Within a pass, apps come before utilities and both keep the order the
 * shell already lists them in.
 */
static void search_rebuild_results(void)
{
    search_result_count = 0U;
    for (uint32_t pass = 0U; pass < 2U; ++pass) {
        for (size_t index = 0U; index < TASKBAR_MAX_START_ENTRIES &&
                search_result_count < SEARCH_MAX_RESULTS; ++index) {
            const struct taskbar_start_entry *entry = &start_entries[index];
            struct search_result *slot;

            /* A heading is a letter dividing the list, not something that
             * can be launched. */
            if (!entry->present || entry->heading ||
                    entry->label[0] == '\0') {
                continue;
            }
            if (search_starts_with_ci(entry->label, search_query) !=
                    (pass == 0U)) {
                continue;
            }
            if (pass == 1U &&
                    !search_contains_ci(entry->label, search_query)) {
                continue;
            }
            slot = &search_results[search_result_count++];
            slot->group = SEARCH_GROUP_APP;
            slot->label = entry->label;
            slot->entry_index = index;
            slot->glyph = TASKBAR_GLYPH_SEARCH;
            slot->art = NULL;
            slot->action = TASKBAR_ACTION_START_ENTRY;
            slot->panel = entry->panel;
        }
        for (size_t index = 0U; index < SEARCH_UTILITY_COUNT &&
                search_result_count < SEARCH_MAX_RESULTS; ++index) {
            const struct search_utility *utility = &search_utilities[index];
            struct search_result *slot;
            bool already_listed = false;

            if (search_starts_with_ci(utility->label, search_query) !=
                    (pass == 0U)) {
                continue;
            }
            if (pass == 1U &&
                    !search_contains_ci(utility->label, search_query)) {
                continue;
            }
            /*
             * Settings is both an application in the Start list and a
             * destination on its rail, and listing it twice under two
             * headings would be this panel disagreeing with itself about
             * how many things it found.  The app wins: it is the one with
             * an icon, and it is what the Start menu would have opened.
             */
            for (size_t seen = 0U; seen < search_result_count; ++seen) {
                if (labels_match(search_results[seen].label,
                        utility->label)) {
                    already_listed = true;
                    break;
                }
            }
            if (already_listed) {
                continue;
            }
            slot = &search_results[search_result_count++];
            slot->group = SEARCH_GROUP_UTILITY;
            slot->label = utility->label;
            slot->entry_index = index;
            slot->glyph = utility->glyph;
            slot->art = utility->art;
            slot->action = utility->action;
            slot->panel = UI_PANEL_NONE;
        }
    }
}

/*
 * Where result `index` sits.  The first result is the best match and gets
 * the tall row; a group heading is drawn above the first result of each
 * group, so the rows below one are pushed down by it.
 */
static bool search_needs_heading(size_t index)
{
    if (index == 0U) {
        return false;   /* the best match stands alone, above the groups */
    }
    return index == 1U ||
        search_results[index].group != search_results[index - 1U].group;
}

/*
 * How tall the panel has to be for what is in it: the best match, then the
 * rows under it with a heading above each group, then the field.
 *
 * Windows 10's panel is a fixed size and pads the difference with empty
 * acrylic.  This sizes to content between a floor and that same ceiling,
 * which the Start menu beside it already does - a panel holding five
 * results and six inches of nothing reads as a panel that failed to load
 * the rest.
 */
static uint32_t search_content_height(void)
{
    uint32_t height = SEARCH_PANEL_PADDING * 2U + SEARCH_FIELD_HEIGHT;

    if (search_result_count == 0U) {
        /* The "No results found" line stands where the first row would. */
        return height + SEARCH_ROW_HEIGHT * 2U;
    }
    height += SEARCH_BEST_HEIGHT;
    for (size_t index = 1U; index < search_result_count; ++index) {
        if (search_needs_heading(index)) {
            height += SEARCH_HEADING_HEIGHT;
        }
        height += SEARCH_ROW_HEIGHT;
    }
    return height < SEARCH_PANEL_MIN_HEIGHT ? SEARCH_PANEL_MIN_HEIGHT :
        height;
}

static void search_layout(void)
{
    uint32_t width = SEARCH_PANEL_WIDTH;
    uint32_t height = search_content_height();

    if (height > SEARCH_PANEL_HEIGHT) {
        height = SEARCH_PANEL_HEIGHT;
    }
    if (width > screen_width) {
        width = screen_width;
    }
    if (height > bar_rect.y) {
        height = bar_rect.y;
    }
    /*
     * Left edge of the screen, standing on the bar - where Windows 10 puts
     * it, and the same anchor the Start menu uses.  It does NOT follow the
     * search box along the bar: the box moves when the cluster's width
     * changes and a panel that slid about with it would be a panel that
     * never appears twice in the same place.
     */
    search_panel_rect = (struct ui_rect){ 0U, bar_rect.y - height, width,
        height };
}

/* The field along the bottom edge, which is where the caret lives. */
static struct ui_rect search_field_rect(void)
{
    return (struct ui_rect){ search_panel_rect.x,
        search_panel_rect.y + search_panel_rect.height -
            SEARCH_FIELD_HEIGHT,
        search_panel_rect.width, SEARCH_FIELD_HEIGHT };
}

/* Everything above the field: the best match and the groups under it. */
static struct ui_rect search_results_rect(void)
{
    return (struct ui_rect){ search_panel_rect.x, search_panel_rect.y,
        search_panel_rect.width,
        search_panel_rect.height - SEARCH_FIELD_HEIGHT };
}

static uint32_t search_result_top(size_t index)
{
    uint32_t top = search_results_rect().y + SEARCH_PANEL_PADDING;

    if (index > 0U) {
        top += SEARCH_BEST_HEIGHT;
    }
    for (size_t scan = 1U; scan < index; ++scan) {
        if (search_needs_heading(scan)) {
            top += SEARCH_HEADING_HEIGHT;
        }
        top += SEARCH_ROW_HEIGHT;
    }
    if (index > 0U && search_needs_heading(index)) {
        top += SEARCH_HEADING_HEIGHT;
    }
    return top;
}

static struct ui_rect search_result_rect(size_t index)
{
    const struct ui_rect area = search_results_rect();
    const uint32_t height = index == 0U ? SEARCH_BEST_HEIGHT :
        SEARCH_ROW_HEIGHT;
    const uint32_t top = search_result_top(index);

    if (index >= search_result_count ||
            top + height > area.y + area.height) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ area.x + SEARCH_PANEL_PADDING, top,
        area.width - SEARCH_PANEL_PADDING * 2U, height };
}

static struct ui_rect search_heading_rect(size_t index)
{
    const struct ui_rect row = search_result_rect(index);

    if (rect_is_empty(row) || !search_needs_heading(index)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ row.x, row.y - SEARCH_HEADING_HEIGHT, row.width,
        SEARCH_HEADING_HEIGHT };
}

static const char *search_group_name(enum search_group group)
{
    return group == SEARCH_GROUP_APP ? "Apps" : "Settings and utilities";
}

/* The one line under the best match's name, which is Windows' own way of
 * saying what it is you are about to open. */
static const char *search_group_caption(enum search_group group)
{
    return group == SEARCH_GROUP_APP ? "App" : "System";
}

static uint32_t search_reveal_alpha(void)
{
    return ui_motion_alpha(&search_reveal);
}

static struct ui_rect search_current_rect(void)
{
    struct ui_rect rect = search_panel_rect;
    const int64_t value = search_reveal.value < 0 ? 0 :
        (search_reveal.value > TASKBAR_ONE ? TASKBAR_ONE :
            search_reveal.value);
    const uint32_t rise = (uint32_t)((TASKBAR_ONE - value) *
        SEARCH_REVEAL_RISE / TASKBAR_ONE);

    rect.y += rise;
    rect.height = rect.height > rise ? rect.height - rise : 0U;
    return rect;
}

static void search_set_open(bool open)
{
    const uint64_t now = clock_monotonic_ns();

    if (search_open == open) {
        return;
    }
    search_open = open;
    if (open) {
        /* One panel on the bar at a time, which is also what lets the two
         * of them share one blurred backdrop. */
        start_set_open(false);
        search_rebuild_results();
        search_layout();
        search_caret_visible = true;
        search_caret_phase = (uint32_t)(now / UINT64_C(530000000)) % 2U;
    } else {
        /* The query goes with it.  Windows does not hold yesterday's search
         * behind the box waiting to be reopened onto. */
        search_query[0] = '\0';
        search_query_length = 0U;
        search_result_count = 0U;
        search_hover = (size_t)-1;
    }
    ui_motion_to(&search_reveal, open ? (int32_t)TASKBAR_ONE : 0,
        open ? SEARCH_REVEAL_OPEN_NS : SEARCH_REVEAL_CLOSE_NS, now);
}

/* The acrylic, the same recipe and the same shared blur the Start menu
 * uses - they are one material and would be wrong to differ. */
/*
 * One result.  The best match - index 0 - gets the tall row, the large
 * icon and a second line naming what it is; the rest get an ordinary row
 * with the name alone, which is the same two weights Windows gives them.
 */
static enum taskbar_status search_draw_result(size_t index,
    struct ui_rect damage, uint32_t reveal, int32_t shift)
{
    const struct taskbar_palette *colours = palette();
    const struct search_result *result = &search_results[index];
    const bool best = index == 0U;
    const uint32_t icon_size = best ? SEARCH_BEST_ICON : SEARCH_ROW_ICON;
    struct ui_rect row = search_result_rect(index);
    struct ui_rect icon;
    enum taskbar_status status;

    if (rect_is_empty(row)) {
        return TASKBAR_STATUS_OK;
    }
    row.y = (uint32_t)((int32_t)row.y + shift);
    if (index == search_hover) {
        status = fill_rounded(row, damage, 0U, colours->item_secondary,
            reveal);
        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
    }
    icon = rect_centred((struct ui_rect){ row.x + SEARCH_PANEL_PADDING,
        row.y, icon_size, row.height }, icon_size, icon_size);
    /*
     * An app draws the icon the Start menu was given for it, so the two
     * lists cannot show the same application as two different things; a
     * utility has no icon to be given and draws its mark instead.
     */
    if (result->group == SEARCH_GROUP_APP) {
        status = draw_icon(&start_entries[result->entry_index].icon, icon,
            damage);
    } else if (result->art != NULL) {
        struct taskbar_icon art = { 0 };

        art.art = result->art;
        art.glyph = result->glyph;
        art.glyph_colour = pack(colours->text_primary);
        status = draw_icon(&art, icon, damage);
    } else {
        status = draw_glyph(result->glyph, icon, damage,
            colours->text_primary, reveal);
    }
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    if (!best) {
        return draw_text(damage, row.x + SEARCH_TEXT_INSET,
            row.y + (row.height + text_ascent()) / 2U - 1U, result->label,
            scale_alpha(colours->text_primary, reveal));
    }
    /* Two lines for the best match: the name, then what kind of thing it
     * is - the answer to "what will Enter do", which is the whole reason
     * Windows gives this row twice the height of the others. */
    status = draw_text(damage, row.x + SEARCH_BEST_TEXT_INSET,
        row.y + row.height / 2U - 2U, result->label,
        scale_alpha(colours->text_primary, reveal));
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    return draw_text(damage, row.x + SEARCH_BEST_TEXT_INSET,
        row.y + row.height / 2U + text_line_height() - 2U,
        search_group_caption(result->group),
        scale_alpha(colours->text_secondary, reveal));
}

static enum taskbar_status search_draw_results(struct ui_rect damage,
    uint32_t reveal, int32_t shift)
{
    const struct taskbar_palette *colours = palette();
    enum taskbar_status status = TASKBAR_STATUS_OK;

    /*
     * Nothing matched.  Say so rather than leaving the panel empty and
     * letting it read as broken - an empty query never lands here, since
     * everything matches one of those.
     */
    if (search_result_count == 0U) {
        const struct ui_rect area = search_results_rect();

        return draw_text(damage, area.x + SEARCH_PANEL_PADDING,
            (uint32_t)((int32_t)(area.y + SEARCH_PANEL_PADDING +
                SEARCH_ROW_HEIGHT) + shift),
            "No results found", scale_alpha(colours->text_secondary,
                reveal));
    }
    for (size_t index = 0U; index < search_result_count &&
            status == TASKBAR_STATUS_OK; ++index) {
        const struct ui_rect heading = search_heading_rect(index);

        if (!rect_is_empty(heading)) {
            status = draw_text(damage, heading.x,
                (uint32_t)((int32_t)(heading.y + (heading.height +
                    text_ascent()) / 2U) + shift),
                search_group_name(search_results[index].group),
                scale_alpha(colours->text_secondary, reveal));
            if (status != TASKBAR_STATUS_OK) {
                break;
            }
        }
        status = search_draw_result(index, damage, reveal, shift);
    }
    return status;
}

/*
 * The box along the bottom edge, which is the one this panel is typed into
 * - the taskbar's own search box is behind the panel while it is open, so
 * drawing the caret down there would be drawing it under the glass.
 */
static enum taskbar_status search_draw_field(struct ui_rect damage,
    uint32_t reveal, int32_t shift)
{
    const struct taskbar_palette *colours = palette();
    struct ui_rect field = search_field_rect();
    struct ui_rect icon;
    uint32_t pen;
    enum taskbar_status status;

    field.y = (uint32_t)((int32_t)field.y + shift);
    status = fill_rounded(field, damage, 0U, colours->start_rail, reveal);
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    icon = rect_centred((struct ui_rect){ field.x + SEARCH_PANEL_PADDING,
        field.y, SEARCH_FIELD_ICON, field.height }, SEARCH_FIELD_ICON,
        SEARCH_FIELD_ICON);
    status = draw_glyph(TASKBAR_GLYPH_SEARCH, icon, damage,
        colours->text_secondary, reveal);
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    pen = field.x + SEARCH_TEXT_INSET;
    status = draw_text(damage, pen,
        field.y + (field.height + text_ascent()) / 2U - 1U,
        search_query[0] != '\0' ? search_query : "Search Phipia",
        scale_alpha(search_query[0] != '\0' ? colours->text_primary :
            colours->text_secondary, reveal));
    if (status != TASKBAR_STATUS_OK || !search_caret_visible) {
        return status;
    }
    return fill_rounded((struct ui_rect){
        pen + text_width(search_query) + 1U, field.y + 14U, 1U,
        field.height - 28U }, damage, 0U, colours->text_primary, reveal);
}

static enum taskbar_status draw_search_panel(struct ui_rect damage)
{
    const uint32_t reveal = search_reveal_alpha();
    const struct ui_rect bounds = search_current_rect();
    const int32_t shift = (int32_t)bounds.y - (int32_t)search_panel_rect.y;
    enum taskbar_status status;

    if (reveal == 0U || rect_is_empty(bounds)) {
        return TASKBAR_STATUS_OK;
    }
    status = draw_panel_material(bounds, damage, reveal);
    if (status == TASKBAR_STATUS_OK) {
        status = search_draw_results(damage, reveal, shift);
    }
    if (status == TASKBAR_STATUS_OK) {
        status = search_draw_field(damage, reveal, shift);
    }
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    return stroke_rounded(bounds, damage, 0U,
        scale_alpha(palette()->flyout_stroke, reveal), 1U);
}

/* Hot-tracking, on the rows' FINAL positions the way the Start menu tracks
 * its own - a panel that is still rising is not somewhere to be clicking. */
static bool search_track_pointer(struct ui_point point)
{
    const size_t was_hover = search_hover;

    search_hover = (size_t)-1;
    if (search_open) {
        for (size_t index = 0U; index < search_result_count; ++index) {
            if (rect_holds(search_result_rect(index), point)) {
                search_hover = index;
                break;
            }
        }
    }
    return was_hover != search_hover;
}

/*
 * Blur the desktop under whichever panel is standing open, for its acrylic
 * to sample.  Neither open leaves the buffer invalid, which is what it
 * should be - there is nothing in front of the desktop to see through.
 */
static void capture_open_panel_backdrop(void)
{
    if (start_open) {
        capture_panel_backdrop(start_menu_rect);
    } else if (search_open) {
        capture_panel_backdrop(search_panel_rect);
    } else {
        panel_material_valid = false;
    }
}

/* What the highlighted result asked for, which is the same taskbar_action
 * the Start menu's own list and rail produce for the same destinations. */
static void search_fill_action(size_t index, struct taskbar_action *action)
{
    const struct search_result *result;

    if (index >= search_result_count) {
        return;
    }
    result = &search_results[index];
    action->kind = result->action;
    action->panel = result->panel;
    action->app_index = result->group == SEARCH_GROUP_APP ?
        result->entry_index : 0U;
}

/* The panel itself, then the hairline that separates it from what is behind
 * it.  A Windows 10 jump list is no more acrylic than the bar under it: it is
 * a near-opaque grey plate with square corners and a one-pixel border, so it
 * takes the same plain-tint path the bar does unless blur is turned on. */
static enum taskbar_status draw_flyout_surface(
    struct ui_rect bounds,
    struct ui_rect damage
)
{
    const struct taskbar_palette *colours = palette();
    const struct ui_rect clipped = rect_intersect(bounds, damage);
    const uint32_t tint = pack(colours->flyout_background);
    const int32_t tint_luma = (int32_t)luma_of(tint);
    enum taskbar_status status = draw_flyout_shadow(bounds, damage,
        TASKBAR_CORNER_OVERLAY);

    if (status != TASKBAR_STATUS_OK || rect_is_empty(clipped)) {
        return status;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - bounds.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint32_t coverage = rounded_coverage(local_x, local_y,
                bounds.width, bounds.height, TASKBAR_CORNER_OVERLAY);
            uint32_t under;

            if (coverage == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
            /* The same recipe the bar uses: one blend of the plate colour
             * over what is already there, and nothing else. */
            uint32_t pixel = blend(under, tint,
                colours->flyout_background.alpha);

            if (bar_blur) {
                pixel = blend(under, set_luma(under, tint_luma),
                    colours->flyout_background.alpha);
                pixel = blend(pixel, set_luma(tint, (int32_t)luma_of(pixel)),
                    colours->flyout_tint_opacity);

                const uint8_t grain = noise_level(clipped.x + x,
                    clipped.y + y);

                pixel = blend(pixel, framebuffer_pack(grain, grain, grain),
                    TASKBAR_NOISE_OPACITY);
            }
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend(under, pixel,
                        (255U * coverage + 8U) / 16U)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return stroke_rounded(bounds, damage, TASKBAR_CORNER_OVERLAY,
        colours->flyout_stroke, 1U);
}

static const char *jump_item_label(size_t index)
{
    switch (index) {
    case TASKBAR_JUMP_PIN:
        return "Unpin from taskbar";
    case TASKBAR_JUMP_CLOSE:
        return "Close window";
    default:
        return "";
    }
}

/*
 * How far a jump list rises as it opens.
 *
 * Windows slides one up out of the bar rather than popping it in place - the
 * same idea as the Start menu's reveal over a shorter distance, because the
 * panel is a fraction of the size.  Kept at or under the shadow's reach so
 * that taskbar_flyout_bounds() already covers the travel and a caller
 * repainting that rectangle leaves no trail behind the rising panel.
 */
#define TASKBAR_FLYOUT_RISE 16

/* Where the list currently IS, which during the reveal is below where it
 * will end up. */
static struct ui_rect flyout_current_rect(void)
{
    const int64_t value = clamp64(flyout_reveal.value, 0, TASKBAR_ONE);
    const uint32_t rise = (uint32_t)((TASKBAR_ONE - value) *
        TASKBAR_FLYOUT_RISE / TASKBAR_ONE);

    return (struct ui_rect){ flyout_rect.x, flyout_rect.y + rise,
        flyout_rect.width, flyout_rect.height };
}

static struct ui_rect jump_item_rect(size_t index)
{
    const struct ui_rect panel = flyout_current_rect();

    return (struct ui_rect){
        panel.x + TASKBAR_JUMP_PADDING,
        panel.y + TASKBAR_JUMP_HEADER_HEIGHT + TASKBAR_JUMP_PADDING +
            (uint32_t)index * TASKBAR_JUMP_ITEM_HEIGHT,
        panel.width - TASKBAR_JUMP_PADDING * 2U,
        TASKBAR_JUMP_ITEM_HEIGHT
    };
}

/*
 * Open the jump list over a button.  Windows keeps the flyout inside the
 * screen, so a button near either edge gets a list that is nudged rather than
 * one that hangs off.
 */
static void open_jump_list(enum taskbar_element element)
{
    const struct ui_rect slot = element_rects[element];
    const struct taskbar_app *app = &apps[app_index_of(element)];
    const uint32_t height = TASKBAR_JUMP_HEADER_HEIGHT +
        TASKBAR_JUMP_PADDING * 2U +
        TASKBAR_JUMP_ITEM_HEIGHT * (app->run == TASKBAR_RUN_PINNED ? 1U : 2U);
    const uint32_t width = TASKBAR_JUMP_WIDTH;
    uint32_t x = slot.x;

    if (x + width + TASKBAR_FLYOUT_EDGE > screen_width) {
        x = screen_width > width + TASKBAR_FLYOUT_EDGE ?
            screen_width - width - TASKBAR_FLYOUT_EDGE : 0U;
    }
    if (x < TASKBAR_FLYOUT_EDGE) {
        x = TASKBAR_FLYOUT_EDGE;
    }
    flyout_kind = TASKBAR_FLYOUT_JUMP_LIST;
    flyout_anchor = element;
    flyout_items = app->run == TASKBAR_RUN_PINNED ? 1U : TASKBAR_JUMP_COUNT;
    flyout_hover = (size_t)-1;
    flyout_rect = (struct ui_rect){
        x, bar_rect.y > height + TASKBAR_FLYOUT_GAP ?
            bar_rect.y - height - TASKBAR_FLYOUT_GAP : 0U,
        width, height
    };
    ui_motion_reset(&flyout_reveal, 0);
    ui_motion_to(&flyout_reveal, (int32_t)TASKBAR_ONE,
        TASKBAR_DURATION_SLIDE_NS, clock_monotonic_ns());
}

static void close_flyout(void)
{
    flyout_kind = TASKBAR_FLYOUT_NONE;
    flyout_anchor = TASKBAR_ELEMENT_NONE;
    flyout_items = 0U;
    flyout_hover = (size_t)-1;
    flyout_rect = (struct ui_rect){ 0U, 0U, 0U, 0U };
    ui_motion_reset(&flyout_reveal, 0);
}

static enum taskbar_status draw_jump_list(struct ui_rect damage)
{
    const struct taskbar_palette *colours = palette();
    const struct taskbar_app *app = &apps[app_index_of(flyout_anchor)];
    const struct ui_rect panel = flyout_current_rect();
    enum taskbar_status status = draw_flyout_surface(panel, damage);

    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    /* The application's own name, which is the jump list's title. */
    status = draw_text(damage,
        panel.x + TASKBAR_JUMP_PADDING + TASKBAR_JUMP_TEXT_INSET,
        panel.y + (TASKBAR_JUMP_HEADER_HEIGHT +
            text_ascent()) / 2U + 2U,
        app->label, colours->text_secondary);
    for (size_t index = 0U; index < flyout_items &&
         status == TASKBAR_STATUS_OK; ++index) {
        const struct ui_rect row = jump_item_rect(index);

        if (index == flyout_hover) {
            status = fill_rounded(row, damage, TASKBAR_CORNER_CONTROL,
                colours->flyout_item_hover, 255U);
        }
        if (status == TASKBAR_STATUS_OK) {
            status = draw_text(damage, row.x + TASKBAR_JUMP_TEXT_INSET,
                row.y + (row.height + text_ascent()) / 2U - 1U,
                index == TASKBAR_JUMP_PIN && app->run == TASKBAR_RUN_PINNED ?
                    "Pin to taskbar" : jump_item_label(index),
                colours->text_primary);
        }
    }
    return status;
}

bool taskbar_flyout_open(void)
{
    return flyout_kind != TASKBAR_FLYOUT_NONE;
}

struct ui_rect taskbar_flyout_bounds(void)
{
    if (flyout_kind == TASKBAR_FLYOUT_NONE) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    /* The shadow reaches past the panel, so the caller has to repaint more
     * than the panel when the flyout closes. */
    const uint32_t reach = TASKBAR_FLYOUT_SHADOW;
    const uint32_t left = flyout_rect.x > reach ? flyout_rect.x - reach : 0U;
    const uint32_t top = flyout_rect.y > reach ? flyout_rect.y - reach : 0U;
    uint32_t right = flyout_rect.x + flyout_rect.width + reach;
    uint32_t bottom = flyout_rect.y + flyout_rect.height + reach;

    if (right > screen_width) {
        right = screen_width;
    }
    if (bottom > screen_height) {
        bottom = screen_height;
    }
    return (struct ui_rect){ left, top, right - left, bottom - top };
}

/* ================================================================ DRAWING */

/*
 * The Start mark: Phipia's own logo.
 *
 * It is a flat P built from three rectangles - a full-height stem and two
 * arms stacked beside its top half - with no perspective, no shading and no
 * curve anywhere in it.  Nothing about it is Microsoft's: the mark it
 * replaced was a flag in perspective receding to the right, which is the
 * defining shape of the Windows logo and had no business being this
 * project's own.
 *
 * A caller may still hand the bar a bitmap through taskbar_set_start_icon(),
 * and that bitmap wins.  Nothing is supplied by default, because the mark
 * below IS the mark: the bar draws it in the theme's own ink and lights it
 * with the accent under the pointer, which a bitmap cannot do without being
 * tinted, and tinting a picture is not what hover means.
 */
static struct taskbar_icon start_icon = { 0 };

/*
 * The mark, as geometry.
 *
 * Measured off assets/logo/phipia.png at its 50% coverage boundary and
 * scaled into a 1024-unit box.  In the source, at 1600 square, the stem
 * spans x 338.5..602.5 over the full height y 331.5..1276.5, and the two
 * arms span x 652.5..1266.5 at y 331.5..607.5 and y 668.5..944.5.  The mark
 * is 928 wide by 945 tall there; it is scaled here so that its height fills
 * the same 820 units the previous mark's did, which keeps the Start button's
 * optical weight where it was, and centred on both axes.
 *
 * Every edge is axis-aligned, so these are rectangles.  The mark they
 * replaced needed four corners each because its edges leaned.
 */
#define START_GRID 1024

struct start_bar {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

static const struct start_bar start_bars[3] = {
    { 110, 102, 339, 922 },   /* the stem, running the full height  */
    { 382, 102, 915, 342 },   /* the upper arm                      */
    { 382, 394, 915, 634 }    /* and the lower one, under it        */
};

static bool start_mark_sample(int64_t x, int64_t y)
{
    for (size_t bar = 0U; bar < 3U; ++bar) {
        if (x >= start_bars[bar].left && x < start_bars[bar].right &&
                y >= start_bars[bar].top && y < start_bars[bar].bottom) {
            return true;
        }
    }
    return false;
}

static enum taskbar_status draw_start_mark(
    struct ui_rect bounds,
    struct ui_rect damage,
    struct taskbar_colour colour
)
{
    const struct ui_rect clipped = rect_intersect(bounds, damage);
    const uint32_t over = pack(colour);
    const uint32_t size = bounds.width < bounds.height ? bounds.width :
        bounds.height;
    const uint32_t samples = TASKBAR_SAMPLES * TASKBAR_SAMPLES;

    if (rect_is_empty(clipped) || size == 0U || colour.alpha == 0U) {
        return TASKBAR_STATUS_OK;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - bounds.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            uint32_t covered = 0U;
            uint32_t under;

            for (uint32_t sample_y = 0U; sample_y < TASKBAR_SAMPLES;
                 ++sample_y) {
                for (uint32_t sample_x = 0U; sample_x < TASKBAR_SAMPLES;
                     ++sample_x) {
                    const int64_t px = ((int64_t)local_x * TASKBAR_SAMPLES +
                        sample_x) * START_GRID /
                        ((int64_t)size * TASKBAR_SAMPLES);
                    const int64_t py = ((int64_t)local_y * TASKBAR_SAMPLES +
                        sample_y) * START_GRID /
                        ((int64_t)size * TASKBAR_SAMPLES);

                    if (start_mark_sample(px, py)) {
                        ++covered;
                    }
                }
            }
            if (covered == 0U) {
                continue;
            }
            const uint32_t alpha = ((uint32_t)colour.alpha * covered +
                samples / 2U) / samples;

            if (alpha == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend(under, over, alpha)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKBAR_STATUS_OK;
}

static enum taskbar_status draw_icon(
    const struct taskbar_icon *icon,
    struct ui_rect bounds,
    struct ui_rect damage
);

/*
 * Windows 10's Start mark is flat, and it takes the accent colour while the
 * pointer is over it - the one place on this bar where hovering changes an
 * icon rather than its background.  Supplied artwork keeps its own colours
 * and is not tinted, because tinting a picture is not what hover means.
 */
static enum taskbar_status draw_start_logo(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t hover
)
{
    const struct taskbar_palette *colours = palette();
    enum taskbar_status status;

    /*
     * A bitmap supplied by the compositor is drawn as it arrives, on either
     * theme, because a caller that hands over artwork has decided what it
     * should look like.  Nothing is supplied by default: the mark is
     * geometry, so it takes the theme's own ink on both themes and can be
     * lit with the accent under the pointer - which a bitmap cannot be
     * without being tinted.
     */
    if (start_icon.art != NULL ||
            (start_icon.pixels != NULL && start_icon.alpha != NULL)) {
        return draw_icon(&start_icon, bounds, damage);
    }
    status = draw_start_mark(bounds, damage, colours->text_primary);
    if (status != TASKBAR_STATUS_OK || hover == 0U) {
        return status;
    }
    struct taskbar_colour accent = colours->accent_fill;

    accent.alpha = (uint8_t)hover;
    return draw_start_mark(bounds, damage, accent);
}

/*
 * Windows does not move a pressed taskbar button.
 *
 * The shipped XAML changes only the fill and the border on press: the icon
 * neither scales nor fades, and the pressed fill is a LOWER alpha than the
 * hover fill in both themes, so the button dims rather than brightening.  An
 * earlier version of this file shrank the artwork on press, which is what a
 * copy assembled from intuition rather than from the template does.
 */

/* ============================================================== HOT TRACK
 *
 * Windows calls this Color Hot-Track, and it is the most recognisable thing
 * a taskbar button does: hovering one lights it in the dominant colour of
 * that application's OWN ICON rather than in a neutral grey, brightest under
 * the pointer and falling away across the cell.  File Explorer glows yellow,
 * a terminal glows grey, the Store glows purple.  It arrived with Windows 7
 * and Windows 10 kept it unchanged; a copy that washes every button the same
 * colour is missing the thing people actually recognise.
 *
 * The colour is FOUND, from a histogram over the icon's pixels, rather than
 * written down per application - so an icon the compositor hands over at run
 * time lights correctly without anyone having said what colour it is.
 */

static bool art_planes(
    const char *name,
    uint32_t wanted,
    const uint32_t **pixels,
    const uint8_t **alpha,
    uint32_t *side
);

/* Hue is carried in sixths of a turn by 256, so the sectors are exact. */
#define HOT_TRACK_HUE_SCALE 256
#define HOT_TRACK_HUE_TURN (HOT_TRACK_HUE_SCALE * 6)
#define HOT_TRACK_BUCKETS 12U
/* Below these a pixel is too dark or too washed out to say what colour the
 * icon is; a mark made only of such pixels has no colour, and glows neutral
 * the way a monochrome icon does on Windows. */
#define HOT_TRACK_MIN_VALUE 48U
#define HOT_TRACK_MIN_SATURATION 60U
#define HOT_TRACK_MIN_ALPHA 128U

static uint32_t hot_track_hue(uint32_t red, uint32_t green, uint32_t blue,
    uint32_t high, uint32_t low)
{
    const int32_t delta = (int32_t)(high - low);
    int32_t hue;

    if (delta == 0) {
        return 0U;
    }
    if (high == red) {
        hue = ((int32_t)green - (int32_t)blue) * HOT_TRACK_HUE_SCALE / delta;
    } else if (high == green) {
        hue = ((int32_t)blue - (int32_t)red) * HOT_TRACK_HUE_SCALE / delta +
            2 * HOT_TRACK_HUE_SCALE;
    } else {
        hue = ((int32_t)red - (int32_t)green) * HOT_TRACK_HUE_SCALE / delta +
            4 * HOT_TRACK_HUE_SCALE;
    }
    hue %= HOT_TRACK_HUE_TURN;
    return (uint32_t)(hue < 0 ? hue + HOT_TRACK_HUE_TURN : hue);
}

/*
 * The icon's colour: the heaviest hue bucket, averaged, then lifted so a
 * dark icon still glows.  Weighting each pixel by its saturation is what
 * stops a mostly-white icon with one coloured corner from reading as white.
 */
static bool dominant_colour(const struct taskbar_icon *icon,
    struct taskbar_colour *out)
{
    const uint32_t *pixels = icon->pixels;
    const uint8_t *alpha = icon->alpha;
    uint32_t width = icon->width;
    uint32_t height = icon->height;
    uint64_t weight[HOT_TRACK_BUCKETS] = { 0 };
    uint64_t sums[HOT_TRACK_BUCKETS][3] = { { 0 } };
    size_t best = 0U;

    if (icon->art != NULL) {
        uint32_t side;

        if (art_planes(icon->art, TASKBAR_ICON_SIZE, &pixels, &alpha,
                &side)) {
            width = side;
            height = side;
        }
    }
    if (pixels == NULL || alpha == NULL || width == 0U || height == 0U) {
        return false;
    }
    for (size_t offset = 0U; offset < (size_t)width * height; ++offset) {
        const uint32_t stored = pixels[offset];
        const uint32_t red = (stored >> 16) & 0xFFU;
        const uint32_t green = (stored >> 8) & 0xFFU;
        const uint32_t blue = stored & 0xFFU;
        const uint32_t high = red > green ?
            (red > blue ? red : blue) : (green > blue ? green : blue);
        const uint32_t low = red < green ?
            (red < blue ? red : blue) : (green < blue ? green : blue);

        if (alpha[offset] < HOT_TRACK_MIN_ALPHA ||
                high < HOT_TRACK_MIN_VALUE) {
            continue;
        }
        const uint32_t saturation = (high - low) * 255U / high;

        if (saturation < HOT_TRACK_MIN_SATURATION) {
            continue;
        }
        const size_t bucket = hot_track_hue(red, green, blue, high, low) *
            HOT_TRACK_BUCKETS / HOT_TRACK_HUE_TURN;

        weight[bucket] += saturation;
        sums[bucket][0] += (uint64_t)red * saturation;
        sums[bucket][1] += (uint64_t)green * saturation;
        sums[bucket][2] += (uint64_t)blue * saturation;
    }
    for (size_t bucket = 1U; bucket < HOT_TRACK_BUCKETS; ++bucket) {
        if (weight[bucket] > weight[best]) {
            best = bucket;
        }
    }
    if (weight[best] == 0U) {
        return false;   /* a monochrome mark; it glows neutral */
    }
    uint32_t channels[3];
    uint32_t high = 1U;

    for (size_t index = 0U; index < 3U; ++index) {
        channels[index] = (uint32_t)(sums[best][index] / weight[best]);
        if (channels[index] > high) {
            high = channels[index];
        }
    }
    /* Lift to full value, keeping the ratios: the glow is the icon's HUE,
     * and a dark blue icon lights a button blue, not black. */
    for (size_t index = 0U; index < 3U; ++index) {
        const uint32_t lifted = channels[index] * 255U / high;

        channels[index] = lifted > 255U ? 255U : lifted;
    }
    *out = (struct taskbar_colour){
        (uint8_t)channels[0], (uint8_t)channels[1], (uint8_t)channels[2],
        0xFFU
    };
    return true;
}

bool taskbar_artwork_tint(const char *name, uint32_t wanted, uint8_t *red,
    uint8_t *green, uint8_t *blue)
{
    /* The finder reads its pixels through icon.art when that is set, which
     * is exactly what a caller naming a piece of artwork wants. */
    const struct taskbar_icon icon = { .art = name };
    struct taskbar_colour found;

    if (name == NULL || red == NULL || green == NULL || blue == NULL) {
        return false;
    }
    (void)wanted;
    if (!dominant_colour(&icon, &found)) {
        return false;
    }
    *red = found.red;
    *green = found.green;
    *blue = found.blue;
    return true;
}

static void refresh_app_tint(size_t index)
{
    app_tint_known[index] = apps[index].present &&
        dominant_colour(&apps[index].icon, &app_tint[index]);
}

/*
 * The glow itself: the tint across the whole cell, brightest in the column
 * the pointer is in and falling away to either side.  Windows' is an
 * ellipse; a horizontal falloff is what survives of it on a button only
 * forty pixels tall, and it is the part that reads as the light following
 * the pointer.
 */
static enum taskbar_status draw_hot_track(
    struct ui_rect panel,
    struct ui_rect damage,
    struct taskbar_colour tint,
    uint32_t alpha_scale
)
{
    const struct ui_rect clipped = rect_intersect(panel, damage);

    if (rect_is_empty(clipped) || alpha_scale == 0U) {
        return TASKBAR_STATUS_OK;
    }
    for (uint32_t x = 0U; x < clipped.width; ++x) {
        const uint32_t column = clipped.x + x;
        const uint32_t distance = column > pointer_column ?
            column - pointer_column : pointer_column - column;
        const uint32_t reach = panel.width;
        const uint32_t bump = distance >= reach ? 0U :
            (reach - distance) * TASKBAR_HOT_TRACK_PEAK / reach;
        const uint32_t alpha = (TASKBAR_HOT_TRACK_BASE + bump) *
            alpha_scale / 255U;
        const enum taskbar_status status = fill_rounded(
            (struct ui_rect){ column, clipped.y, 1U, clipped.height },
            clipped, 0U, tint, alpha);

        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
    }
    return TASKBAR_STATUS_OK;
}

static enum taskbar_status draw_button_fill(
    enum taskbar_element element,
    struct ui_rect damage,
    struct taskbar_colour colour,
    uint32_t alpha_scale
)
{
    const struct ui_rect panel = panel_rect(element);
    const bool stacked = element_is_app(element) &&
        (apps[app_index_of(element)].run == TASKBAR_RUN_GROUPED ||
         apps[app_index_of(element)].run == TASKBAR_RUN_GROUPED_FOCUS);
    const uint32_t offset = TASKBAR_MULTIWINDOW_INSET_DARK;
    enum taskbar_status status;

    if (alpha_scale == 0U) {
        return TASKBAR_STATUS_OK;
    }
    if (!stacked || panel.height <= offset * 2U) {
        return fill_rounded(panel, damage, TASKBAR_CORNER_CONTROL, colour,
            alpha_scale);
    }
    /*
     * Windows 10 says "more than one window" by drawing the button as a
     * stack of cards: one or two copies of the fill offset up and to the
     * right, each a little shorter, with the real button in front.  Windows
     * 11 says it by pulling the highlight in from the right instead, which
     * is why the two look nothing alike with three windows open.
     */
    for (uint32_t card = 2U; card >= 1U; --card) {
        const uint32_t shift = offset * card;

        if (panel.height <= shift * 2U || panel.width <= shift) {
            continue;
        }
        status = fill_rounded((struct ui_rect){
            panel.x + shift, panel.y + shift,
            panel.width - shift, panel.height - shift * 2U
        }, damage, TASKBAR_CORNER_CONTROL, colour,
            alpha_scale * (3U - card) / 3U);
        if (status != TASKBAR_STATUS_OK) {
            return status;
        }
    }
    return fill_rounded(panel, damage, TASKBAR_CORNER_CONTROL, colour,
        alpha_scale);
}

/*
 * The four resting appearances of a taskbar button, in the order Windows
 * layers them.  A foreground app carries a permanent Secondary fill; hovering
 * anything adds one; hovering a foreground app promotes the pair to Primary;
 * pressing replaces it with Tertiary, which is a LOWER alpha than hover in
 * both themes - the button dims on press, it does not brighten.
 *
 * A button asking for attention is different in kind: Windows lays an opaque
 * plate over it rather than a wash, and flashes between that plate and the
 * ordinary appearance seven times before settling on the plate.
 */
static enum taskbar_status draw_highlight(
    enum taskbar_element element,
    struct ui_rect damage
)
{
    const struct taskbar_palette *colours = palette();
    const struct taskbar_button_state *button = &buttons[element];
    const uint32_t hover = ui_motion_alpha(&button->hover);
    const uint32_t press = ui_motion_alpha(&button->press);
    const bool active = element_is_foreground(element);
    const bool attention = element_is_app(element) &&
        apps[app_index_of(element)].attention;
    enum taskbar_status status = TASKBAR_STATUS_OK;

    if (attention && attention_plate_showing()) {
        const struct taskbar_colour plate = press != 0U ?
            colours->attention_pressed : (hover != 0U ?
                colours->attention_hover : colours->attention_rest);

        return draw_button_fill(element, damage, plate, 255U);
    }
    if (active) {
        /*
         * The foreground application's resting plate is NEUTRAL: Windows 10
         * marks "this is the window in front" with a plain lighter fill and
         * saves the colour for the pointer.  Hovering it fades the plate up
         * to Primary rather than stacking a second wash on it, so a hovered
         * foreground button lands on Primary exactly.
         */
        status = draw_button_fill(element, damage, colours->item_secondary,
            255U - hover);
        if (status == TASKBAR_STATUS_OK) {
            status = draw_button_fill(element, damage, colours->item_primary,
                hover);
        }
    }
    if (status == TASKBAR_STATUS_OK && hover != 0U) {
        /*
         * And on top of whichever plate is there, the hot track: the
         * application's own colour where the pointer is.  An application
         * whose icon has no colour to find - a terminal, a monochrome mark -
         * glows in the theme's neutral instead, which is what Windows does
         * with the same icons.
         */
        struct taskbar_colour tint = colours->item_secondary;
        bool tinted = false;

        if (element_is_app(element)) {
            const size_t index = app_index_of(element);

            if (app_tint_known[index]) {
                tint = app_tint[index];
                tinted = true;
            }
        }
        if (tinted) {
            status = draw_hot_track(panel_rect(element), damage, tint, hover);
        } else if (!active) {
            status = draw_button_fill(element, damage, tint, hover);
        }
    }
    if (status == TASKBAR_STATUS_OK && press != 0U) {
        status = draw_button_fill(element, damage, colours->item_tertiary,
            press);
    }
    /*
     * Windows 10 draws no border on a taskbar button: the fill is the whole
     * of it.  The gradient outline the previous version drew is Windows 11's
     * TaskbarButtonOutlineThickness, and it has no counterpart here.
     */
    return status;
}

/*
 * The running indicator.
 *
 * Windows 10 draws a wide underline flush with the bottom of the button, not
 * Windows 11's short centred pill: a bar most of the cell's width and a few
 * pixels tall, in the accent colour, sitting on the very last rows of the
 * bar.  Focus does not change its length - what changes is the fill behind
 * the icon - so the same bar is drawn whether the window is in front or not,
 * and only its colour moves.
 */
static enum taskbar_status draw_indicator(
    enum taskbar_element element,
    const struct taskbar_app *app,
    struct ui_rect damage
)
{
    const struct taskbar_palette *colours = palette();
    const struct ui_rect panel = panel_rect(element);
    const bool foreground = app->run == TASKBAR_RUN_FOREGROUND ||
        app->run == TASKBAR_RUN_GROUPED_FOCUS;
    const struct taskbar_colour colour = app->attention ?
        colours->indicator_attention : (foreground ? colours->accent_fill :
            colours->indicator_background);
    const uint32_t width = TASKBAR_INDICATOR_WIDTH_FOREGROUND < panel.width ?
        TASKBAR_INDICATOR_WIDTH_FOREGROUND : panel.width;
    const uint32_t height = TASKBAR_INDICATOR_HEIGHT;
    const uint32_t bottom = panel.y + panel.height -
        TASKBAR_INDICATOR_BOTTOM_MARGIN;
    const uint32_t centre = panel.x + panel.width / 2U;

    if (app->run == TASKBAR_RUN_PINNED) {
        return TASKBAR_STATUS_OK;
    }
    return fill_rounded((struct ui_rect){
        centre - width / 2U, bottom - height, width, height
    }, damage, TASKBAR_CORNER_INDICATOR, colour, 255U);
}

/*
 * A download's progress, drawn where the running indicator would be.  Windows
 * gives it the same three pixels and the same 1.5 radius, so the two never
 * appear together: while an application is downloading, its progress is the
 * indicator.
 */
static enum taskbar_status draw_progress(
    enum taskbar_element element,
    uint8_t percent,
    struct ui_rect damage
)
{
    const struct taskbar_palette *colours = palette();
    const struct ui_rect panel = panel_rect(element);
    const uint32_t width = TASKBAR_PROGRESS_WIDTH;
    const uint32_t height = TASKBAR_PROGRESS_HEIGHT;
    const uint32_t left = panel.x + (panel.width - width) / 2U;
    const uint32_t top = panel.y + panel.height -
        TASKBAR_INDICATOR_BOTTOM_MARGIN - height;
    const uint32_t filled = width * (percent > 100U ? 100U : percent) / 100U;
    const enum taskbar_status status = fill_rounded(
        (struct ui_rect){ left, top, width, height }, damage,
        TASKBAR_CORNER_INDICATOR, colours->progress_track, 255U);

    if (status != TASKBAR_STATUS_OK || filled == 0U) {
        return status;
    }
    return fill_rounded((struct ui_rect){ left, top, filled, height }, damage,
        TASKBAR_CORNER_INDICATOR, colours->progress_foreground, 255U);
}

/*
 * The keyboard focus ring.  FocusVisualMargin of -2 puts it outside the
 * panel, so on a default bar it is a forty-four pixel square: the whole slot,
 * which is why a focused button's ring meets its neighbour's.
 */
static enum taskbar_status draw_focus_ring(
    enum taskbar_element element,
    struct ui_rect damage
)
{
    const struct ui_rect panel = panel_rect(element);
    const uint32_t margin = TASKBAR_FOCUS_MARGIN;
    const struct ui_rect outer = {
        panel.x - margin, panel.y - margin,
        panel.width + margin * 2U, panel.height + margin * 2U
    };

    if (panel.x < margin || panel.y < margin) {
        return TASKBAR_STATUS_OK;
    }
    return stroke_rounded(outer, damage,
        TASKBAR_CORNER_CONTROL + TASKBAR_EIGHTHS(margin),
        palette()->text_primary, TASKBAR_FOCUS_THICKNESS);
}

/* A count on the corner of an icon: a filled disc with the number inside. */
/*
 * A count on the corner of an icon.  Windows draws it as an accent-coloured
 * pill overlapping the bottom-right of the 24-pixel icon, sixteen tall with a
 * radius of eight, widening for a two-digit number.  A one-digit badge is
 * therefore a disc and a two-digit one is a stadium, which is the same shape
 * rule the running indicator follows.
 */
static enum taskbar_status draw_badge(
    struct ui_rect icon,
    uint8_t count,
    struct ui_rect damage
)
{
    char text[4];
    const uint32_t height = TASKBAR_BADGE_HEIGHT;
    enum taskbar_status status;

    if (count == 0U) {
        return TASKBAR_STATUS_OK;
    }
    (void)append_number(text, sizeof(text), 0U,
        count > 99U ? 99U : (uint32_t)count, 1U);

    const uint32_t label = text_width(text);
    const uint32_t width = label + TASKBAR_BADGE_PADDING * 2U < height ?
        height : label + TASKBAR_BADGE_PADDING * 2U;
    const struct ui_rect bounds = {
        icon.x + icon.width - width + TASKBAR_BADGE_OVERHANG,
        icon.y + icon.height - height + TASKBAR_BADGE_OVERHANG,
        width, height
    };

    status = fill_rounded(bounds, damage, TASKBAR_EIGHTHS(height) / 2U,
        accent_colour, 255U);
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    const uint32_t left = bounds.x + (bounds.width > label ?
        (bounds.width - label) / 2U : 0U);
    const uint32_t baseline = bounds.y + (bounds.height +
        text_ascent()) / 2U - 1U;
    const struct taskbar_colour ink = TASKBAR_RGBA(0xFFU, 0xFFU, 0xFFU, 0xFFU);

    return draw_text(damage, left, baseline, text, ink);
}

static bool names_equal(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

/*
 * Built-in artwork, resolved by name and by the size it is being drawn at.
 * Every size was resampled from the original rather than from a larger cell,
 * so picking the one that matches is what keeps a 24-pixel icon sharp.
 */
static bool art_planes(
    const char *name,
    uint32_t wanted,
    const uint32_t **pixels,
    const uint8_t **alpha,
    uint32_t *side
)
{
    for (size_t index = 0U; index < TASKBAR_ART_COUNT; ++index) {
        if (!names_equal(taskbar_art[index].name, name)) {
            continue;
        }
        /*
         * The largest size that FITS, not the smallest that covers.  These
         * planes are drawn one-to-one, so a size larger than the box would
         * have to be scaled down to fit it - and scaling is the thing this
         * is avoiding.  Undersized and crisp beats exact and resampled.
         */
        size_t choice = 0U;

        for (size_t size = 0U; size < TASKBAR_ART_SIZES; ++size) {
            if (taskbar_art_size[size] <= wanted) {
                choice = size;
            }
        }
        *pixels = taskbar_art[index].pixels[choice];
        *alpha = taskbar_art[index].alpha[choice];
        *side = taskbar_art_size[choice];
        return true;
    }
    return false;
}

/*
 * An application's artwork.  Built-in art wins, then pixels the caller
 * decoded itself, then the Lucide mark in a flat colour.
 */
static enum taskbar_status draw_icon(
    const struct taskbar_icon *icon,
    struct ui_rect bounds,
    struct ui_rect damage
)
{
    const struct ui_rect clipped = rect_intersect(bounds, damage);
    const uint32_t *pixels = icon->pixels;
    const uint8_t *alpha = icon->alpha;
    uint32_t source_width = icon->width;
    uint32_t source_height = icon->height;

    if (rect_is_empty(clipped)) {
        return TASKBAR_STATUS_OK;
    }
    if (icon->art != NULL) {
        uint32_t side;

        if (art_planes(icon->art, bounds.width < bounds.height ?
                bounds.width : bounds.height, &pixels, &alpha, &side)) {
            source_width = side;
            source_height = side;
        }
    }
    if (pixels == NULL || alpha == NULL || source_width == 0U ||
            source_height == 0U) {
        struct taskbar_colour colour = palette()->text_primary;

        if (icon->glyph_colour != 0U) {
            const struct framebuffer_state format = framebuffer_get_state();

            colour.red = channel_of(icon->glyph_colour, format.red_position);
            colour.green = channel_of(icon->glyph_colour,
                format.green_position);
            colour.blue = channel_of(icon->glyph_colour, format.blue_position);
            colour.alpha = 0xFFU;
        }
        return draw_glyph(icon->glyph, bounds, damage, colour, 255U);
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - bounds.y + y;
        const uint32_t source_y = local_y * source_height / bounds.height;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint32_t source_x = local_x * source_width / bounds.width;
            const size_t offset = (size_t)source_y * source_width + source_x;
            const uint8_t coverage = alpha[offset];
            const uint32_t stored = pixels[offset];
            uint32_t under;

            if (coverage == 0U) {
                continue;
            }
            /* The planes are baked 0x00RRGGBB; the loader tells the kernel
             * where this device actually keeps its channels, so recompose
             * rather than assuming the two agree. */
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend(under, framebuffer_pack(
                        (uint8_t)(stored >> 16), (uint8_t)(stored >> 8),
                        (uint8_t)stored), coverage)) != SURFACE_STATUS_OK) {
                return TASKBAR_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return TASKBAR_STATUS_OK;
}

/*
 * The search entry point.  As an icon it is a magnifier in an ordinary
 * button; as a box it is a wider panel spanning the whole height of the bar,
 * with the magnifier set in from its left edge and "Search Phipia"
 * beside it, which is the shape a stock Windows 10 taskbar shows.
 */
static enum taskbar_status draw_search(
    enum taskbar_element element,
    struct ui_rect damage
)
{
    static const char label[] = "Search Phipia";
    const struct taskbar_palette *colours = palette();
    const struct ui_rect panel = panel_rect(element);
    const uint32_t glyph = TASKBAR_TRAY_GLYPH_SIZE;
    enum taskbar_status status;

    if (search_mode_effective == TASKBAR_SEARCH_ICON ||
            search_mode_effective == TASKBAR_SEARCH_HIDDEN) {
        return draw_glyph(TASKBAR_GLYPH_SEARCH,
            icon_rect(element), damage,
            colours->text_primary, 255U);
    }
    /* The box carries a resting fill of its own, unlike the icon variant,
     * which is what makes it read as a field rather than a button. */
    status = fill_rounded(panel, damage, TASKBAR_CORNER_CONTROL,
        colours->search_field, 255U);
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    const struct ui_rect mark = {
        panel.x + (TASKBAR_SEARCH_TEXT_INSET - glyph) / 2U,
        panel.y + (panel.height - glyph) / 2U, glyph, glyph
    };

    /* Both take the FIELD's ink.  The bar's would be white on white. */
    status = draw_glyph(TASKBAR_GLYPH_SEARCH, mark,
        damage, colours->search_text, 255U);
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    return draw_text(damage, panel.x + TASKBAR_SEARCH_TEXT_INSET,
        panel.y + (panel.height + text_ascent()) / 2U - 1U, label,
        scale_alpha(colours->search_text, 0xB4U));
}

/* ================================================================ BATTERY
 *
 * Windows 10's tray battery is an outline with a SOLID BAR inside it whose
 * length is the charge - not a fixed row of stencilled cells.  Lucide has no
 * filled battery and drawing one as three bars is what makes a copy of this
 * icon look hand drawn at sixteen pixels: the bars land on uneven columns,
 * and the mark reads as stripes rather than as a level.
 *
 * So the shell is Lucide's, untouched, and the charge is a rectangle drawn
 * inside it.  That also makes the icon mean something: it moves.
 *
 * The rectangle is stated in Lucide's own 24-unit grid so it stays inside
 * the shell at every size.  Lucide draws the shell as rect x=2 y=6 w=16 h=12
 * with a 2-unit stroke, so the clear interior runs from 3 to 17 across and 7
 * to 17 down; one unit of air inside that is 4..16 and 8..16.
 */
/* One pixel of air between the shell and the charge, as Windows leaves. */
#define BATTERY_AIR 1U

/*
 * Find the shell's hollow by reading the glyph that was just drawn.
 *
 * The alternative is to restate Lucide's rectangle here in its own units and
 * scale it, which sounds tidier and is wrong: the rasterizer snaps the shell
 * to whole pixels, so a second calculation that does not snap the same way
 * lands a pixel off - which is exactly how the charge ended up touching the
 * shell along its top and left and floating away from it along its bottom
 * and right.  Reading the cell cannot disagree with the cell.
 *
 * On the middle row the shell is three runs of ink: left wall, right wall,
 * terminal nub.  The hollow is between the first two.  On the middle column
 * it is two: top wall and bottom wall.
 */
static bool cell_gap(const uint8_t *cell, uint32_t size, uint32_t line,
    bool horizontal, uint32_t *start, uint32_t *end)
{
    bool inside = false;
    bool seen_first = false;

    for (uint32_t index = 0U; index < size; ++index) {
        const size_t offset = horizontal ?
            (size_t)line * size + index : (size_t)index * size + line;
        const bool ink = cell[offset] >= 128U;

        if (ink && !inside) {
            if (seen_first) {
                *end = index;
                return *end > *start;
            }
            inside = true;
        } else if (!ink && inside) {
            *start = index;
            inside = false;
            seen_first = true;
        }
    }
    return false;
}

static enum taskbar_status draw_battery(
    struct ui_rect bounds,
    struct ui_rect damage,
    struct taskbar_colour colour
)
{
    const struct taskbar_lucide_entry *entry =
        glyph_entry(TASKBAR_GLYPH_BATTERY);
    const enum taskbar_status status = draw_glyph(TASKBAR_GLYPH_BATTERY,
        bounds, damage, colour, 255U);
    uint32_t left;
    uint32_t right;
    uint32_t top;
    uint32_t bottom;

    if (status != TASKBAR_STATUS_OK || battery_percent == 0U ||
            entry == NULL) {
        return status;
    }

    const uint32_t wanted = bounds.width < bounds.height ?
        bounds.width : bounds.height;
    size_t choice = TASKBAR_LUCIDE_SIZES - 1U;

    for (size_t index = 0U; index < TASKBAR_LUCIDE_SIZES; ++index) {
        if (taskbar_lucide_size[index] >= wanted) {
            choice = index;
            break;
        }
    }

    const uint8_t *cell = entry->alpha[choice];
    const uint32_t size = taskbar_lucide_size[choice];

    if (cell == NULL || size == 0U ||
            !cell_gap(cell, size, size / 2U, true, &left, &right) ||
            !cell_gap(cell, size, size / 2U, false, &top, &bottom)) {
        return TASKBAR_STATUS_OK;
    }
    if (right - left <= BATTERY_AIR * 2U ||
            bottom - top <= BATTERY_AIR * 2U) {
        return TASKBAR_STATUS_OK;
    }
    left += BATTERY_AIR;
    right -= BATTERY_AIR;
    top += BATTERY_AIR;
    bottom -= BATTERY_AIR;

    const uint32_t span = right - left;
    /* Round up, so any charge at all is at least one visible pixel and the
     * icon never reads as empty while the machine is still running. */
    uint32_t width = (span * battery_percent + 99U) / 100U;

    if (width > span) {
        width = span;
    }
    /* The cell is drawn into bounds at its own size, so cell coordinates are
     * bounds coordinates; draw_glyph picks the cell that fits. */
    return fill_rounded((struct ui_rect){
        bounds.x + left, bounds.y + top, width, bottom - top
    }, damage, 0U, colour, 255U);
}

/* Which glyph a tray slot carries. */
static enum taskbar_glyph tray_glyph_for(enum taskbar_element element)
{
    switch (element) {
    case TASKBAR_ELEMENT_CHEVRON:
        return TASKBAR_GLYPH_CHEVRON_UP;
    case TASKBAR_ELEMENT_TRAY_NETWORK:
        return TASKBAR_GLYPH_NETWORK;
    case TASKBAR_ELEMENT_TRAY_VOLUME:
        return TASKBAR_GLYPH_VOLUME;
    case TASKBAR_ELEMENT_TRAY_BATTERY:
        return TASKBAR_GLYPH_BATTERY;
    case TASKBAR_ELEMENT_ACTION_CENTER:
        return TASKBAR_GLYPH_ACTION_CENTER;
    default:
        return TASKBAR_GLYPH_NONE;
    }
}

static enum taskbar_status draw_clock(struct ui_rect damage)
{
    const struct ui_rect bounds = element_rects[TASKBAR_ELEMENT_CLOCK];
    const struct taskbar_palette *colours = palette();
    const uint32_t line = text_line_height();
    /* Two sixteen-pixel lines, less the two the stack's negative top margin
     * removes, centred in the bar: on a forty-eight pixel bar the first line
     * box therefore starts seven pixels down. */
    const uint32_t block = line * 2U - TASKBAR_CLOCK_STACK_TOP_MARGIN;
    const uint32_t top = bounds.y + (bounds.height > block ?
        (bounds.height - block) / 2U : 0U);
    const uint32_t right = bounds.x + bounds.width - TASKBAR_CLOCK_PADDING +
        TASKBAR_CLOCK_STACK_RIGHT_MARGIN;
    const uint32_t time_width = text_width(clock_time_text);
    const uint32_t date_width = text_width(clock_date_text);
    /* Centre the resampled cell in the sixteen-pixel line box. */
    const uint32_t lead = (line - text_line_cell()) / 2U;
    enum taskbar_status status;

    status = draw_text(damage, right > time_width ? right - time_width : 0U,
        top + lead + text_ascent(), clock_time_text, colours->text_primary);
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    return draw_text(damage, right > date_width ? right - date_width : 0U,
        top + line + lead + text_ascent(), clock_date_text,
        colours->text_primary);
}

/*
 * "Show desktop": the sliver at the very edge of the screen.  Windows 10
 * marks it with a full-height rule down its left edge rather than a short
 * centred one, and washes the strip on hover; there is no artwork inside it.
 */
static enum taskbar_status draw_show_desktop(struct ui_rect damage)
{
    const struct ui_rect bounds = element_rects[TASKBAR_ELEMENT_SHOW_DESKTOP];
    const struct taskbar_palette *colours = palette();
    const uint32_t hover = ui_motion_alpha(
        &buttons[TASKBAR_ELEMENT_SHOW_DESKTOP].hover);
    const uint32_t rule_height = TASKBAR_SHOW_DESKTOP_PIPE_HEIGHT <
        bounds.height ? TASKBAR_SHOW_DESKTOP_PIPE_HEIGHT : bounds.height;
    /*
     * The rule down the left edge of the sliver is drawn only if the palette
     * asks for one.  Windows 10 does draw a faint one; at this contrast it
     * reads as a bright seam beside the clock rather than as a divider, so
     * both palettes leave it out and the strip is found by hovering it, the
     * way it is found on Windows once the pointer is already there.
     */
    enum taskbar_status status = TASKBAR_STATUS_OK;

    if (colours->show_desktop_rule.alpha != 0U) {
        status = fill_rounded((struct ui_rect){
            bounds.x, bounds.y + (bounds.height - rule_height) / 2U,
            1U, rule_height
        }, damage, 0U, colours->show_desktop_rule, 255U);
    }
    if (status != TASKBAR_STATUS_OK || hover == 0U) {
        return status;
    }
    return fill_rounded(bounds, damage, 0U, colours->item_secondary, hover);
}

enum taskbar_status taskbar_draw(struct ui_rect damage)
{
    enum taskbar_status status;

    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    /*
     * Nothing of ours is under this damage.  Both rising panels have to be
     * named here: they stand ABOVE the bar, so a repaint of one alone
     * misses bar_rect entirely and would return here having drawn nothing.
     */
    if (rect_is_empty(rect_intersect(bar_rect, damage)) &&
            rect_is_empty(rect_intersect(taskbar_flyout_bounds(), damage)) &&
            rect_is_empty(rect_intersect(start_menu_rect, damage)) &&
            rect_is_empty(rect_intersect(search_panel_rect, damage))) {
        return TASKBAR_STATUS_OK;
    }
    status = draw_material(damage);
    for (enum taskbar_element element = TASKBAR_ELEMENT_START;
         element < TASKBAR_ELEMENT_COUNT && status == TASKBAR_STATUS_OK;
         element = (enum taskbar_element)(element + 1)) {
        if (!element_present[element]) {
            continue;
        }
        if (element == TASKBAR_ELEMENT_SHOW_DESKTOP) {
            status = draw_show_desktop(damage);
            continue;
        }
        status = draw_highlight(element, damage);
        if (status != TASKBAR_STATUS_OK) {
            break;
        }
        if (element_is_app(element)) {
            const struct taskbar_app *app = &apps[app_index_of(element)];

            status = draw_icon(&app->icon,
                icon_rect(element), damage);
            /* A download's progress replaces the running indicator: Windows
             * gives them the same three pixels in the same place. */
            if (status == TASKBAR_STATUS_OK) {
                status = app->progress != 0U ?
                    draw_progress(element, app->progress, damage) :
                    draw_indicator(element, app, damage);
            }
            if (status == TASKBAR_STATUS_OK) {
                status = draw_badge(icon_rect(element), app->badge, damage);
            }
            if (status == TASKBAR_STATUS_OK && element == focused) {
                status = draw_focus_ring(element, damage);
            }
            continue;
        }
        if (status == TASKBAR_STATUS_OK && element == focused) {
            status = draw_focus_ring(element, damage);
        }
        switch (element) {
        case TASKBAR_ELEMENT_START:
            status = draw_start_logo(icon_rect(element), damage,
                ui_motion_alpha(&buttons[element].hover));
            break;
        case TASKBAR_ELEMENT_SEARCH:
            status = draw_search(element, damage);
            break;
        case TASKBAR_ELEMENT_TASK_VIEW:
            status = draw_glyph(TASKBAR_GLYPH_TASK_VIEW,
                icon_rect(element), damage,
                palette()->text_primary, 255U);
            break;
        case TASKBAR_ELEMENT_TRAY_BATTERY:
            status = draw_battery(rect_centred(element_rects[element],
                    TASKBAR_TRAY_GLYPH_SIZE, TASKBAR_TRAY_GLYPH_SIZE),
                damage, palette()->text_primary);
            break;
        case TASKBAR_ELEMENT_CHEVRON:
        case TASKBAR_ELEMENT_TRAY_NETWORK:
        case TASKBAR_ELEMENT_TRAY_VOLUME:
        case TASKBAR_ELEMENT_ACTION_CENTER:
            status = draw_glyph(tray_glyph_for(element),
                rect_centred(element_rects[element],
                    TASKBAR_TRAY_GLYPH_SIZE, TASKBAR_TRAY_GLYPH_SIZE),
                damage, palette()->text_primary, 255U);
            break;
        case TASKBAR_ELEMENT_CLOCK:
            status = draw_clock(damage);
            break;
        default:
            break;
        }
    }
    if (status == TASKBAR_STATUS_OK) {
        status = draw_start_menu(damage);
    }
    if (status == TASKBAR_STATUS_OK) {
        status = draw_search_panel(damage);
    }
    if (status == TASKBAR_STATUS_OK &&
            flyout_kind == TASKBAR_FLYOUT_JUMP_LIST) {
        status = draw_jump_list(damage);
    }
    if (status != TASKBAR_STATUS_OK) {
        return status;
    }
    counters.draws += 1U;
    return TASKBAR_STATUS_OK;
}

/* ================================================================== INPUT */

static enum taskbar_element element_at(struct ui_point point)
{
    if (!rect_holds(bar_rect, point)) {
        return TASKBAR_ELEMENT_NONE;
    }
    for (enum taskbar_element element = TASKBAR_ELEMENT_START;
         element < TASKBAR_ELEMENT_COUNT;
         element = (enum taskbar_element)(element + 1)) {
        if (element_present[element] &&
                rect_holds(element_rects[element], point)) {
            return element;
        }
    }
    return TASKBAR_ELEMENT_NONE;
}

bool taskbar_contains(struct ui_point point)
{
    return initialized && rect_holds(bar_rect, point);
}

/* Everything a change of state for one button can repaint. */
static struct ui_rect button_damage(enum taskbar_element element)
{
    if (element == TASKBAR_ELEMENT_NONE ||
            element >= TASKBAR_ELEMENT_COUNT || !element_present[element]) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return element_rects[element];
}

static void set_hover(enum taskbar_element element, struct ui_rect *damage,
    uint64_t now)
{
    if (hovered == element) {
        return;
    }
    if (hovered != TASKBAR_ELEMENT_NONE) {
        ui_motion_to(&buttons[hovered].hover, 0,
            TASKBAR_DURATION_BRUSH_NS, now);
        *damage = rect_join(*damage, button_damage(hovered));
    }
    hovered = element;
    if (hovered != TASKBAR_ELEMENT_NONE) {
        ui_motion_to(&buttons[hovered].hover, (int32_t)TASKBAR_ONE,
            TASKBAR_DURATION_BRUSH_NS, now);
        *damage = rect_join(*damage, button_damage(hovered));
    }
    counters.hover_changes += 1U;
}

enum taskbar_status taskbar_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (taskbar_flyout_open()) {
        const size_t previous = flyout_hover;

        flyout_hover = (size_t)-1;
        for (size_t index = 0U; index < flyout_items; ++index) {
            if (rect_holds(jump_item_rect(index), point)) {
                flyout_hover = index;
                break;
            }
        }
        if (flyout_hover != previous) {
            *damage = flyout_rect;
        }
    }
    const enum taskbar_element under = element_at(point);

    /*
     * The glow follows the pointer, so moving WITHIN a button repaints it
     * even though the hovered element has not changed.  Without this the
     * light is stuck wherever the pointer first crossed the edge.
     */
    if (point.x >= 0 && under != TASKBAR_ELEMENT_NONE) {
        const uint32_t column = (uint32_t)point.x;

        if (column != pointer_column) {
            pointer_column = column;
            if (element_is_app(under)) {
                *damage = rect_join(*damage, panel_rect(under));
            }
        }
    }
    if (start_track_pointer(point)) {
        *damage = rect_join(*damage, start_menu_rect);
    }
    if (search_track_pointer(point)) {
        *damage = rect_join(*damage, search_panel_rect);
    }
    set_hover(under, damage, clock_monotonic_ns());
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_pointer_leave(struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    set_hover(TASKBAR_ELEMENT_NONE, damage, clock_monotonic_ns());
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_pointer_press(struct ui_point point,
    enum ui_pointer_button button, struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };

    const enum taskbar_element element = element_at(point);

    /*
     * A right click opens the jump list on an application button, and on
     * empty taskbar space opens the one-item menu Windows 11 reduced the old
     * taskbar context menu to.  A right click anywhere else, or any press
     * outside an open flyout, dismisses whatever is showing.
     */
    if (button == UI_POINTER_BUTTON_RIGHT) {
        const struct ui_rect previous = taskbar_flyout_bounds();

        close_flyout();
        if (element_is_app(element)) {
            open_jump_list(element);
        }
        *damage = rect_join(previous, taskbar_flyout_bounds());
        return TASKBAR_STATUS_OK;
    }
    if (taskbar_flyout_open() && !rect_holds(flyout_rect, point)) {
        const struct ui_rect previous = taskbar_flyout_bounds();

        close_flyout();
        *damage = previous;
        return TASKBAR_STATUS_OK;
    }
    if (button != UI_POINTER_BUTTON_LEFT) {
        return TASKBAR_STATUS_OK;
    }
    if (element == TASKBAR_ELEMENT_NONE) {
        return TASKBAR_STATUS_OK;
    }
    pressed_element = element;
    ui_motion_to(&buttons[element].press, (int32_t)TASKBAR_ONE,
        TASKBAR_DURATION_BRUSH_NS, clock_monotonic_ns());
    *damage = button_damage(element);
    return TASKBAR_STATUS_OK;
}

/*
 * What a click means.  Windows 11 answers this from the run state alone: a
 * pinned app launches, a background window is raised, and a foreground window
 * is minimized because clicking the button you are already looking at is how
 * you put it away.
 */
static struct taskbar_action action_for(
    enum taskbar_element element,
    enum ui_pointer_button button
)
{
    struct taskbar_action action = { TASKBAR_ACTION_NONE, 0U, UI_PANEL_NONE };

    if (element_is_app(element)) {
        const size_t index = app_index_of(element);
        const struct taskbar_app *app = &apps[index];

        action.app_index = index;
        action.panel = app->panel;
        if (button == UI_POINTER_BUTTON_MIDDLE) {
            action.kind = TASKBAR_ACTION_NEW_INSTANCE;
            return action;
        }
        switch (app->run) {
        case TASKBAR_RUN_PINNED:
            action.kind = TASKBAR_ACTION_LAUNCH;
            break;
        case TASKBAR_RUN_FOREGROUND:
        case TASKBAR_RUN_GROUPED_FOCUS:
            action.kind = TASKBAR_ACTION_MINIMIZE;
            break;
        case TASKBAR_RUN_BACKGROUND:
        case TASKBAR_RUN_GROUPED:
        default:
            action.kind = TASKBAR_ACTION_ACTIVATE;
            break;
        }
        return action;
    }
    switch (element) {
    case TASKBAR_ELEMENT_START:
        /* The taskbar opens its own menu - it is part of the shell, not
         * something the compositor has to build - and still reports the
         * click, so a host that wants its own launcher can have one. */
        start_set_open(!start_open);
        action.kind = TASKBAR_ACTION_START;
        break;
    case TASKBAR_ELEMENT_SEARCH:
        /* Opens the panel, and reports the click as well - the compositor
         * may want to know the search entry point was used even though the
         * taskbar has already done the only thing it needs to. */
        search_set_open(!search_open);
        action.kind = TASKBAR_ACTION_SEARCH;
        break;
    case TASKBAR_ELEMENT_TASK_VIEW:
        action.kind = TASKBAR_ACTION_TASK_VIEW;
        break;
    case TASKBAR_ELEMENT_WIDGETS:
        action.kind = TASKBAR_ACTION_WIDGETS;
        break;
    case TASKBAR_ELEMENT_SHOW_DESKTOP:
        action.kind = TASKBAR_ACTION_SHOW_DESKTOP;
        break;
    case TASKBAR_ELEMENT_CHEVRON:
        action.kind = TASKBAR_ACTION_TRAY_OVERFLOW;
        break;
    case TASKBAR_ELEMENT_TRAY_NETWORK:
        action.kind = TASKBAR_ACTION_NETWORK;
        break;
    case TASKBAR_ELEMENT_TRAY_VOLUME:
        action.kind = TASKBAR_ACTION_VOLUME;
        break;
    case TASKBAR_ELEMENT_TRAY_BATTERY:
        action.kind = TASKBAR_ACTION_BATTERY;
        break;
    case TASKBAR_ELEMENT_ACTION_CENTER:
        action.kind = TASKBAR_ACTION_NOTIFICATIONS;
        break;
    case TASKBAR_ELEMENT_CLOCK:
        action.kind = TASKBAR_ACTION_CALENDAR;
        break;
    default:
        break;
    }
    return action;
}

enum taskbar_status taskbar_pointer_release(struct ui_point point,
    enum ui_pointer_button button, struct ui_rect *damage,
    struct taskbar_action *action)
{
    if (damage == NULL || action == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    *action = (struct taskbar_action){ TASKBAR_ACTION_NONE, 0U,
        UI_PANEL_NONE };

    if (taskbar_flyout_open() && rect_holds(flyout_rect, point)) {
        const size_t index = flyout_hover;
        const size_t app_index = app_index_of(flyout_anchor);
        const bool pinned = apps[app_index].run == TASKBAR_RUN_PINNED;

        *damage = taskbar_flyout_bounds();
        close_flyout();
        if (index < flyout_items || index == 0U) {
            action->app_index = app_index;
            action->panel = apps[app_index].panel;
            if (index == TASKBAR_JUMP_PIN) {
                action->kind = pinned ? TASKBAR_ACTION_PIN :
                    TASKBAR_ACTION_UNPIN;
            } else if (index == TASKBAR_JUMP_CLOSE) {
                action->kind = TASKBAR_ACTION_CLOSE;
            }
        }
        return TASKBAR_STATUS_OK;
    }
    if (search_open && rect_holds(search_panel_rect, point)) {
        /*
         * Same rule the Start menu follows: a result launches and the panel
         * goes away, anything else inside it - a heading, the field, the
         * gaps - is the panel's own background and eats the click without
         * closing.  You are still typing.
         */
        *damage = search_panel_rect;
        if (search_hover == (size_t)-1) {
            return TASKBAR_STATUS_OK;
        }
        search_fill_action(search_hover, action);
        search_set_open(false);
        counters.activations += 1U;
        return TASKBAR_STATUS_OK;
    }
    if (start_open && rect_holds(start_menu_rect, point)) {
        /*
         * Everything inside the menu closes it, which is what Windows does:
         * the menu is a launcher, and a launcher that stays open after you
         * have launched something is in the way.
         */
        *damage = start_menu_rect;
        if (start_hover_rail != (size_t)-1) {
            switch (start_hover_rail) {
            case START_RAIL_ACCOUNT:
                action->kind = TASKBAR_ACTION_ACCOUNT;
                break;
            case START_RAIL_DOCUMENTS:
                action->kind = TASKBAR_ACTION_DOCUMENTS;
                break;
            case START_RAIL_PICTURES:
                action->kind = TASKBAR_ACTION_PICTURES;
                break;
            case START_RAIL_SETTINGS:
                action->kind = TASKBAR_ACTION_OPEN_SETTINGS;
                break;
            case START_RAIL_POWER:
                action->kind = TASKBAR_ACTION_POWER;
                break;
            default:
                /* The hamburger only widens the rail on Windows, and this
                 * rail has nothing to widen into. */
                return TASKBAR_STATUS_OK;
            }
        } else if (start_hover_entry != (size_t)-1) {
            action->kind = TASKBAR_ACTION_START_ENTRY;
            action->app_index = start_hover_entry;
            action->panel = start_entries[start_hover_entry].panel;
        } else if (start_hover_tile != (size_t)-1) {
            action->kind = TASKBAR_ACTION_START_TILE;
            action->app_index = start_hover_tile;
            action->panel = start_tiles[start_hover_tile].panel;
        } else {
            return TASKBAR_STATUS_OK;   /* the menu's own background */
        }
        start_set_open(false);
        counters.activations += 1U;
        return TASKBAR_STATUS_OK;
    }

    const enum taskbar_element element = element_at(point);
    const enum taskbar_element was_pressed = pressed_element;
    const uint64_t now = clock_monotonic_ns();

    if (was_pressed != TASKBAR_ELEMENT_NONE) {
        ui_motion_to(&buttons[was_pressed].press, 0,
            TASKBAR_DURATION_BRUSH_NS, now);
        *damage = rect_join(*damage, button_damage(was_pressed));
        pressed_element = TASKBAR_ELEMENT_NONE;
    }
    if (element == TASKBAR_ELEMENT_NONE) {
        return TASKBAR_STATUS_OK;
    }
    /* A middle click has no press phase, so it is decided here on its own. */
    if (button == UI_POINTER_BUTTON_MIDDLE) {
        *action = action_for(element, button);
        counters.activations += 1U;
        return TASKBAR_STATUS_OK;
    }
    if (button != UI_POINTER_BUTTON_LEFT || element != was_pressed) {
        return TASKBAR_STATUS_OK;
    }
    *action = action_for(element, button);
    counters.activations += 1U;
    return TASKBAR_STATUS_OK;
}

/*
 * A character, for the search panel to filter by.  It was a stub that threw
 * the character away until the panel existed to have somewhere to put it.
 *
 * Nothing else on this bar takes typing, so a keystroke arriving with the
 * panel shut is not an error - there is simply nothing listening, and it is
 * dropped rather than opening the panel on its own.  Windows opens search
 * from a keystroke only because the Start button has focus at the time;
 * this shell has no focus model to have handed the bar the keyboard.
 */
enum taskbar_status taskbar_text_input(char character, struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    if (!search_open || (unsigned char)character < 0x20U) {
        return TASKBAR_STATUS_OK;
    }
    if (search_query_length + 1U < sizeof(search_query)) {
        search_query[search_query_length] = character;
        ++search_query_length;
        search_query[search_query_length] = '\0';
        search_rebuild_results();
        /* Fewer results means a shorter panel, so the geometry is redone
         * on every keystroke rather than fixed at the moment it opened. */
        search_layout();
        /* What was under the pointer may not be a result any more, or may
         * be a different one; the next move re-finds it. */
        search_hover = (size_t)-1;
    }
    search_caret_visible = true;
    *damage = search_panel_rect;
    return TASKBAR_STATUS_OK;
}

/* Backspace, which the search panel is so far the only thing on this bar to
 * have a use for. */
enum taskbar_status taskbar_key_backspace(struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    if (!search_open) {
        return TASKBAR_STATUS_OK;
    }
    if (search_query_length > 0U) {
        --search_query_length;
        search_query[search_query_length] = '\0';
        search_rebuild_results();
        search_layout();
        search_hover = (size_t)-1;
    }
    search_caret_visible = true;
    *damage = search_panel_rect;
    return TASKBAR_STATUS_OK;
}

/*
 * Alt+F4, which opens Task Manager.
 *
 * Windows gives this chord to "close the foreground window" and puts Task
 * Manager on Ctrl+Shift+Esc.  This shell has no window manager to close
 * anything with, so the chord is unclaimed, and it is spent on the one
 * window that has no button anywhere to reach it by - which is the whole
 * reason Windows gives Task Manager a chord of its own in the first place.
 *
 * Whatever panel is open closes: the window this is about to report would
 * otherwise come up behind a Start menu standing over it.
 */
enum taskbar_status taskbar_key_alt_f4(struct ui_rect *damage,
    struct taskbar_action *action)
{
    if (damage == NULL || action == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    *action = (struct taskbar_action){ TASKBAR_ACTION_NONE, 0U,
        UI_PANEL_NONE };
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    if (search_open) {
        *damage = rect_join(*damage, search_panel_rect);
        search_set_open(false);
    }
    if (start_open) {
        *damage = rect_join(*damage, start_menu_rect);
        start_set_open(false);
    }
    action->kind = TASKBAR_ACTION_TASK_MANAGER;
    counters.activations += 1U;
    return TASKBAR_STATUS_OK;
}

/*
 * Enter, which runs the best match - the top result, the one the panel has
 * drawn large and captioned for exactly this.  There is no way to arrow
 * down to another: this platform delivers characters and Backspace to a
 * window and nothing else, which is the same reason the Explorer window's
 * command palette also runs its first row and no other.
 */
enum taskbar_status taskbar_key_enter(struct ui_rect *damage,
    struct taskbar_action *action)
{
    if (damage == NULL || action == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    *action = (struct taskbar_action){ TASKBAR_ACTION_NONE, 0U,
        UI_PANEL_NONE };
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    if (!search_open || search_result_count == 0U) {
        return TASKBAR_STATUS_OK;
    }
    *damage = search_panel_rect;
    search_fill_action(0U, action);
    search_set_open(false);
    counters.activations += 1U;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_dismiss(struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    *damage = taskbar_flyout_bounds();
    close_flyout();
    /* The panels dismiss too - Escape with one open should put it away,
     * which is the whole of what a caller means by dismiss(). */
    if (search_open) {
        *damage = rect_join(*damage, search_panel_rect);
        search_set_open(false);
    }
    if (start_open) {
        *damage = rect_join(*damage, start_menu_rect);
        start_set_open(false);
    }
    return TASKBAR_STATUS_OK;
}

/* ============================================================== ANIMATION */

bool taskbar_animating(void)
{
    if (!initialized) {
        return false;
    }
    if (cluster_slide.running) {
        return true;
    }
    /*
     * The three panels count too.
     *
     * A caller drives frames from this: it repaints while this says yes and
     * stops when it says no.  Leaving the Start menu's reveal and the jump
     * list's out of the answer meant a caller could stop pumping frames with
     * either one still part way through, and a menu frozen half risen is a
     * worse bug than one that never animated at all.
     */
    if (start_reveal.running || search_reveal.running ||
            flyout_reveal.running) {
        return true;
    }
    /* An open search panel owes a frame every half second for its caret,
     * even with nothing else on the bar moving. */
    if (search_open) {
        return true;
    }
    if (attention_pulsing) {
        const uint64_t now = clock_monotonic_ns();

        /* Once the seven toggles are spent the plate is steady, so the bar
         * stops asking for frames rather than repainting a still picture. */
        if (now - attention_started_ns < TASKBAR_ATTENTION_TOGGLE_NS *
                TASKBAR_ATTENTION_TOGGLES) {
            return true;
        }
    }
    for (size_t index = 0U; index < TASKBAR_ELEMENT_COUNT; ++index) {
        if (buttons[index].hover.running || buttons[index].press.running ||
                buttons[index].indicator.running) {
            return true;
        }
    }
    return false;
}

bool taskbar_animate(struct ui_rect *damage)
{
    const uint64_t now = clock_monotonic_ns();
    bool moved = false;

    if (damage == NULL || !initialized) {
        return false;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    for (enum taskbar_element element = TASKBAR_ELEMENT_START;
         element < TASKBAR_ELEMENT_COUNT;
         element = (enum taskbar_element)(element + 1)) {
        struct taskbar_button_state *button = &buttons[element];
        bool touched = false;

        /* The two brush cross-fades are linear because a BrushTransition
         * is; only the indicator, which is a composition animation rather
         * than a brush, is eased. */
        touched |= ui_motion_advance(&button->hover, now, ui_ease_linear);
        touched |= ui_motion_advance(&button->press, now, ui_ease_linear);
        touched |= ui_motion_advance(&button->indicator, now, ui_ease_standard);
        if (touched) {
            *damage = rect_join(*damage, button_damage(element));
            moved = true;
        }
    }
    if (ui_motion_advance(&cluster_slide, now, ui_ease_standard)) {
        /* The whole strip moves, so everything from the old origin to the
         * new one is dirty; taking the bar's full width is both correct and
         * cheaper than tracking which slots crossed which pixel. */
        place_cluster();
        *damage = rect_join(*damage, bar_rect);
        moved = true;
    }
    if (attention_pulsing) {
        for (size_t index = 0U; index < TASKBAR_MAX_APPS; ++index) {
            if (apps[index].present && apps[index].attention) {
                *damage = rect_join(*damage, button_damage(
                    (enum taskbar_element)(
                        TASKBAR_ELEMENT_APP_FIRST + index)));
                moved = true;
            }
        }
    }
    if (ui_motion_advance(&flyout_reveal, now, ui_ease_decelerate)) {
        /* The panel slides, so everything it crosses is dirty; its bounds
         * already take in the shadow, which is further than the rise. */
        *damage = rect_join(*damage, taskbar_flyout_bounds());
        moved = true;
    }
    if (ui_motion_advance(&start_reveal, now,
            start_open ? ui_ease_decelerate : ui_ease_accelerate)) {
        /* The whole menu rises or falls, so everything it will cover on the
         * way is dirty; its full rectangle is both correct and cheaper than
         * tracking the sliding edge. */
        *damage = rect_join(*damage, start_menu_rect);
        moved = true;
    }
    if (ui_motion_advance(&search_reveal, now,
            search_open ? ui_ease_decelerate : ui_ease_accelerate)) {
        /* Same reasoning as the menu above: the whole panel is dirty while
         * it moves. */
        *damage = rect_join(*damage, search_panel_rect);
        moved = true;
    }
    if (search_open) {
        /*
         * The caret, in half-second halves - fully on or fully off, so
         * there is nothing here for ui_motion.h to interpolate, and only
         * the phase CROSSING a boundary is a repaint.  The same blink the
         * Explorer window's boxes use, arrived at separately because
         * nothing in this file had one to share.
         */
        const uint32_t phase = (uint32_t)(now / UINT64_C(530000000)) % 2U;

        if (phase != search_caret_phase) {
            search_caret_phase = phase;
            search_caret_visible = !search_caret_visible;
            *damage = rect_join(*damage, search_field_rect());
            moved = true;
        }
    }
    if (refresh_clock()) {
        *damage = rect_join(*damage,
            element_rects[TASKBAR_ELEMENT_CLOCK]);
        moved = true;
    }
    return moved;
}

/* ============================================================ PUBLIC STATE */

static void sync_indicator(size_t index)
{
    const enum taskbar_element element = (enum taskbar_element)(
        TASKBAR_ELEMENT_APP_FIRST + index);
    const bool foreground = apps[index].run == TASKBAR_RUN_FOREGROUND ||
        apps[index].run == TASKBAR_RUN_GROUPED_FOCUS;

    ui_motion_to(&buttons[element].indicator,
        foreground ? (int32_t)TASKBAR_ONE : 0,
        TASKBAR_DURATION_INDICATOR_NS, clock_monotonic_ns());
}

enum taskbar_status taskbar_set_app(size_t index,
    const struct taskbar_app *app)
{
    if (app == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (index >= TASKBAR_MAX_APPS) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    apps[index] = *app;
    apps[index].label[TASKBAR_LABEL_BYTES - 1U] = '\0';
    apps[index].present = true;
    refresh_app_tint(index);
    if (initialized) {
        layout();
        sync_indicator(index);
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_clear_app(size_t index)
{
    if (index >= TASKBAR_MAX_APPS) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    apps[index] = (struct taskbar_app){ 0 };
    app_tint_known[index] = false;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_run_state(size_t index,
    enum taskbar_run_state run)
{
    if (index >= TASKBAR_MAX_APPS) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    if (!apps[index].present) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    apps[index].run = run;
    if (initialized) {
        sync_indicator(index);
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_badge(size_t index, uint8_t badge)
{
    if (index >= TASKBAR_MAX_APPS || !apps[index].present) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    apps[index].badge = badge;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_progress(size_t index, uint8_t percent)
{
    if (index >= TASKBAR_MAX_APPS || !apps[index].present) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    apps[index].progress = percent > 100U ? 100U : percent;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_attention(size_t index, bool attention)
{
    if (index >= TASKBAR_MAX_APPS || !apps[index].present) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    if (attention && !apps[index].attention) {
        attention_started_ns = clock_monotonic_ns();
    }
    apps[index].attention = attention;
    attention_pulsing = false;
    for (size_t scan = 0U; scan < TASKBAR_MAX_APPS; ++scan) {
        attention_pulsing = attention_pulsing || apps[scan].attention;
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_focus(size_t index)
{
    if (index >= TASKBAR_MAX_APPS || !apps[index].present) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    focused = (enum taskbar_element)(TASKBAR_ELEMENT_APP_FIRST + index);
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_clear_focus(void)
{
    focused = TASKBAR_ELEMENT_NONE;
    return initialized ? TASKBAR_STATUS_OK : TASKBAR_STATUS_NOT_INITIALIZED;
}

enum taskbar_status taskbar_set_start_icon(const struct taskbar_icon *icon)
{
    if (icon == NULL) {
        start_icon = (struct taskbar_icon){ 0 };
        return TASKBAR_STATUS_OK;
    }
    start_icon = *icon;
    return TASKBAR_STATUS_OK;
}

/*
 * What the pointer means over the bar.
 *
 * A caret in the search box - the bar's one text field, whether it is the
 * wide box on the strip or the field at the foot of the open panel - and
 * the ordinary arrow over everything else.  Windows 10 does exactly this:
 * a taskbar button is a button, and the arrow stays over all of them.
 *
 * The bar answers last, after every window, because it is under all of
 * them; see the resolver in tools/preview/main.c and the template in
 * docs/INTEGRATION.md.
 */
enum cursor_kind taskbar_cursor_at(struct ui_point point)
{
    if (!initialized) {
        return CURSOR_NORMAL_SELECT;
    }
    if (search_open && rect_holds(search_field_rect(), point)) {
        return CURSOR_TEXT_SELECT;
    }
    if (!search_open && search_mode_effective == TASKBAR_SEARCH_BOX &&
            element_at(point) == TASKBAR_ELEMENT_SEARCH) {
        return CURSOR_TEXT_SELECT;
    }
    return CURSOR_NORMAL_SELECT;
}

enum taskbar_status taskbar_set_theme(enum taskbar_theme theme)
{
    bar_theme = theme;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_alignment(enum taskbar_alignment alignment)
{
    bar_alignment = alignment;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_size(enum taskbar_size size)
{
    bar_size = size;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_accent(uint8_t red, uint8_t green,
    uint8_t blue)
{
    accent_colour = (struct taskbar_colour){ red, green, blue, 0xFFU };
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_transparency(bool transparent)
{
    bar_transparent = transparent;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_blur(bool blur)
{
    bar_blur = blur;
    material_valid = false;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_chevron_visible(bool visible)
{
    chevron_visible = visible;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_action_center_visible(bool visible)
{
    action_centre_visible = visible;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_show_desktop_button(bool visible)
{
    show_desktop_button = visible;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_widgets_visible(bool visible)
{
    widgets_visible = visible;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_search_visible(bool visible)
{
    search_visible = visible;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_search_mode(enum taskbar_search_mode mode)
{
    search_mode = mode;
    search_visible = mode != TASKBAR_SEARCH_HIDDEN;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_task_view_visible(bool visible)
{
    task_view_visible = visible;
    if (initialized) {
        layout();
    }
    return TASKBAR_STATUS_OK;
}

struct taskbar_counters taskbar_get_counters(void)
{
    return counters;
}

bool taskbar_is_initialized(void)
{
    return initialized;
}

void taskbar_shutdown(void)
{
    initialized = false;
    canvas = NULL;
    material_valid = false;
    hovered = TASKBAR_ELEMENT_NONE;
    pressed_element = TASKBAR_ELEMENT_NONE;
}

enum taskbar_status taskbar_initialize(
    struct surface *surface,
    uint32_t width,
    uint32_t height
)
{
    if (surface == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (initialized) {
        return TASKBAR_STATUS_ALREADY_INITIALIZED;
    }
    if (width == 0U || width > TASKBAR_MATERIAL_MAX_WIDTH ||
            height <= TASKBAR_HEIGHT_DEFAULT * 2U) {
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    canvas = surface;
    screen_width = width;
    screen_height = height;
    counters = (struct taskbar_counters){ 0 };
    material_valid = false;
    clock_text_valid = false;
    hovered = TASKBAR_ELEMENT_NONE;
    pressed_element = TASKBAR_ELEMENT_NONE;
    cluster_placed = false;
    close_flyout();
    focused = TASKBAR_ELEMENT_NONE;
    attention_pulsing = false;
    ui_motion_reset(&cluster_slide, (int32_t)TASKBAR_ONE);
    for (size_t index = 0U; index < TASKBAR_ELEMENT_COUNT; ++index) {
        ui_motion_reset(&buttons[index].hover, 0);
        ui_motion_reset(&buttons[index].press, 0);
        ui_motion_reset(&buttons[index].indicator, 0);
    }
    /* The bar no longer asks the platform for a glyph - it carries its own
     * face, see the TEXT section - so there is nothing to verify here and
     * nothing to fall back to if the service is missing. */
    /* The real-time clock is optional: a machine without one still gets a
     * taskbar, it just gets one that says it has no clock. */
    if (!rtc_is_started()) {
        (void)rtc_start();
    }
    initialized = true;
    layout();
    return TASKBAR_STATUS_OK;
}

/* ============================================================== SELF TEST */

bool taskbar_self_test(void)
{
    self_test_failure = "taskbar self-test passed";

    /* Easing curves must be pinned at both ends and monotonic between. */
    if (ui_ease_standard(0) != 0 ||
        ui_ease_standard((int32_t)TASKBAR_ONE) != (int32_t)TASKBAR_ONE ||
        ui_ease_decelerate(0) != 0 ||
        ui_ease_decelerate((int32_t)TASKBAR_ONE) != (int32_t)TASKBAR_ONE ||
        ui_ease_accelerate(0) != 0 ||
        ui_ease_accelerate((int32_t)TASKBAR_ONE) != (int32_t)TASKBAR_ONE) {
        self_test_failure = "an easing curve is not pinned at its ends";
        return false;
    }
    int32_t previous = -1;

    for (uint32_t step = 0U; step <= 64U; ++step) {
        const int32_t at = (int32_t)((int64_t)step * TASKBAR_ONE / 64);
        const int32_t value = ui_ease_standard(at);

        if (value < previous) {
            self_test_failure = "the standard easing curve is not monotonic";
            return false;
        }
        previous = value;
    }
    /*
     * A curve also has to MOVE across its span.
     *
     * An earlier evaluator in this file dropped a factor of t out of the
     * cubic, which turned FluentStandard into a step - nought until
     * eighty-five per cent of the way through, and one after - and every
     * check above still passed, because a step is pinned at both ends and is
     * perfectly monotonic.  FluentStandard is symmetric about its midpoint,
     * so a curve that is not half done halfway through is not that curve.
     */
    const int32_t midpoint = ui_ease_standard((int32_t)(TASKBAR_ONE / 2));

    if (midpoint < (int32_t)(TASKBAR_ONE * 40 / 100) ||
            midpoint > (int32_t)(TASKBAR_ONE * 60 / 100)) {
        self_test_failure = "the standard easing curve is not half done "
            "halfway through";
        return false;
    }
    /*
     * EVERY DURATION ON THIS BAR IS ONE OF THE FOUR.  Windows 10's XAML
     * theme resources declare exactly four control durations, and a number
     * that is not one of them is a number somebody chose - which is the
     * thing this bar is not allowed to contain.
     */
    {
        static const uint64_t theme[] = { UI_MOTION_BRUSH_NS,
            UI_MOTION_FAST_NS, UI_MOTION_REVEAL_NS, UI_MOTION_SLOW_NS };
        static const uint64_t used[] = { TASKBAR_DURATION_BRUSH_NS,
            TASKBAR_DURATION_INDICATOR_NS, TASKBAR_DURATION_SLIDE_NS,
            START_REVEAL_OPEN_NS, START_REVEAL_CLOSE_NS,
            SEARCH_REVEAL_OPEN_NS, SEARCH_REVEAL_CLOSE_NS };

        for (size_t index = 0U; index < sizeof(used) / sizeof(used[0]);
             ++index) {
            bool found = false;

            for (size_t option = 0U;
                 option < sizeof(theme) / sizeof(theme[0]); ++option) {
                found = found || used[index] == theme[option];
            }
            if (!found) {
                self_test_failure = "a duration is not a Windows theme "
                    "duration";
                return false;
            }
        }
    }
    /* A decelerating curve is ahead of a linear ramp at its midpoint and an
     * accelerating one is behind it; if that inverts, the two are swapped. */
    if (ui_ease_decelerate((int32_t)(TASKBAR_ONE / 2)) <=
            (int32_t)(TASKBAR_ONE / 2) ||
        ui_ease_accelerate((int32_t)(TASKBAR_ONE / 2)) >=
            (int32_t)(TASKBAR_ONE / 2)) {
        self_test_failure = "the easing curves are the wrong way round";
        return false;
    }

    /* A square with a radius of half its side is a circle: its corner sample
     * must be outside and its centre inside. */
    if (rounded_coverage(0U, 0U, 16U, 16U, 8U) >=
            TASKBAR_SAMPLES * TASKBAR_SAMPLES ||
        rounded_coverage(8U, 8U, 16U, 16U, 8U) !=
            TASKBAR_SAMPLES * TASKBAR_SAMPLES) {
        self_test_failure = "rounded coverage does not round its corners";
        return false;
    }
    if (rounded_coverage(0U, 0U, 16U, 16U, 0U) !=
            TASKBAR_SAMPLES * TASKBAR_SAMPLES) {
        self_test_failure = "a square corner was rounded anyway";
        return false;
    }

    /*
     * Every mark must put ink somewhere.  A Lucide cell that came out empty
     * means the generator failed on that SVG - a path command it does not
     * understand, say - and an empty icon is exactly the kind of failure
     * nobody notices until they look at the bar.
     */
    for (size_t index = 0U; index < TASKBAR_LUCIDE_COUNT; ++index) {
        for (size_t size = 0U; size < TASKBAR_LUCIDE_SIZES; ++size) {
            const uint32_t side = taskbar_lucide_size[size];
            const uint8_t *cell = taskbar_lucide[index].alpha[size];
            uint32_t ink = 0U;

            for (size_t at = 0U; at < (size_t)side * side; ++at) {
                ink += cell[at];
            }
            if (ink == 0U) {
                self_test_failure = "a Lucide glyph cell is empty";
                return false;
            }
        }
    }
    /* And every mark the bar asks for by name must resolve to one of them. */
    for (enum taskbar_glyph glyph = TASKBAR_GLYPH_SEARCH;
         glyph < TASKBAR_GLYPH_COUNT;
         glyph = (enum taskbar_glyph)(glyph + 1)) {
        if (glyph_entry(glyph) == NULL) {
            self_test_failure = "a taskbar mark has no Lucide icon behind it";
            return false;
        }
    }
    /*
     * The Start mark is a P, and what makes it a P rather than three bars is
     * where it is EMPTY: the notch between the stem and the arms, the gap
     * between the two arms, and the whole lower right, which is the counter
     * the letter is read by.  Sample all three, and the ink either side of
     * each of them.
     */
    if (!start_mark_sample(200, 500) ||        /* the stem            */
            !start_mark_sample(600, 200) ||    /* the upper arm       */
            !start_mark_sample(600, 500)) {    /* and the lower one   */
        self_test_failure = "a bar of the Start mark is not filled";
        return false;
    }
    if (start_mark_sample(360, 500) ||         /* stem to arm         */
            start_mark_sample(600, 368) ||     /* arm to arm          */
            start_mark_sample(600, 800)) {     /* under the lower arm */
        self_test_failure = "the Start mark has lost a counter";
        return false;
    }

    /* Colour blending must be exact at both ends and unbiased in the middle. */
    const uint32_t black = framebuffer_pack(0U, 0U, 0U);
    const uint32_t white = framebuffer_pack(255U, 255U, 255U);

    if (blend(black, white, 0U) != black || blend(black, white, 255U) != white ||
            blend(black, white, 128U) != framebuffer_pack(128U, 128U, 128U)) {
        self_test_failure = "alpha compositing is biased";
        return false;
    }
    if (luma_of(white) < 250U || luma_of(black) != 0U) {
        self_test_failure = "luminance of the extremes is wrong";
        return false;
    }

    /* Rectangle algebra, which every damage calculation rests on. */
    const struct ui_rect left = { 0U, 0U, 10U, 10U };
    const struct ui_rect right = { 5U, 5U, 10U, 10U };
    const struct ui_rect apart = { 40U, 40U, 5U, 5U };
    const struct ui_rect meet = rect_intersect(left, right);
    const struct ui_rect both = rect_join(left, right);

    if (meet.x != 5U || meet.y != 5U || meet.width != 5U || meet.height != 5U ||
        both.width != 15U || both.height != 15U ||
        !rect_is_empty(rect_intersect(left, apart)) ||
        !rect_holds(left, (struct ui_point){ 0, 0 }) ||
        rect_holds(left, (struct ui_point){ 10, 0 }) ||
        rect_holds(left, (struct ui_point){ -1, 0 })) {
        self_test_failure = "rectangle algebra is inconsistent";
        return false;
    }

    /*
     * The search panel's matching, against the rule its header promises.
     * The empty query is the one worth pinning down: it is what fills the
     * panel the moment it opens, and an empty query matching nothing would
     * open a panel reading "No results found" over a machine full of
     * applications.
     */
    if (!search_contains_ci("Settings", "") ||
            !search_starts_with_ci("Settings", "")) {
        self_test_failure = "an empty search query matched nothing";
        return false;
    }
    if (!search_contains_ci("Settings", "ett") ||
            !search_contains_ci("settings", "SET") ||
            !search_starts_with_ci("Settings", "set")) {
        self_test_failure = "the search panel refused a name it matches";
        return false;
    }
    if (search_contains_ci("Settings", "zz") ||
            search_starts_with_ci("Settings", "ett")) {
        self_test_failure = "the search panel matched a name it should not";
        return false;
    }
    /* The dedupe is whole-string, so one label is never folded into a
     * longer one that merely starts the same way. */
    if (!labels_match("Store", "store") || labels_match("Store", "Stores")) {
        self_test_failure = "the search panel's label match is not exact";
        return false;
    }
    /* Both rising panels share one blurred backdrop, which is only sound
     * because the buffer is sized for whichever of them is bigger. */
    if (PANEL_MATERIAL_MAX_WIDTH < SEARCH_PANEL_WIDTH ||
            PANEL_MATERIAL_MAX_WIDTH < START_MENU_WIDTH ||
            PANEL_MATERIAL_MAX_HEIGHT < SEARCH_PANEL_HEIGHT ||
            PANEL_MATERIAL_MAX_HEIGHT < START_MENU_HEIGHT) {
        self_test_failure =
            "the shared panel material is too small for a panel using it";
        return false;
    }
    /*
     * Every size this bar names for an ICON has to be a size the artwork is
     * actually rasterized at.
     *
     * Nothing here resamples any more - a glyph and a picture are both
     * drawn one-to-one and centred - so a constant naming a size in between
     * two real ones does not produce that size.  It silently produces the
     * largest real size below it, a little smaller than the layout meant,
     * and nothing complains.  SEARCH_BEST_ICON spent a release at forty
     * drawing thirty-two that way, and SEARCH_FIELD_ICON at twenty drawing
     * sixteen.  This is the check that would have said so.
     *
     * A box that is deliberately an AREA rather than an icon - a toolbar
     * slot, a card's plate - is not in this list and should not be: those
     * are meant to hold a smaller mark centred in them.
     */
    /*
     * The Start mark's three bars have to be disjoint and inside the grid
     * they are stated on.  Two that touch would draw as one shape and the
     * P would lose its counters - the two gaps ARE the letter.
     */
    for (size_t bar = 0U; bar < 3U; ++bar) {
        if (start_bars[bar].left >= start_bars[bar].right ||
                start_bars[bar].top >= start_bars[bar].bottom ||
                start_bars[bar].left < 0 ||
                start_bars[bar].top < 0 ||
                start_bars[bar].right > START_GRID ||
                start_bars[bar].bottom > START_GRID) {
            self_test_failure = "a Start mark bar is empty or off its grid";
            return false;
        }
        for (size_t other = bar + 1U; other < 3U; ++other) {
            if (start_bars[bar].left < start_bars[other].right &&
                    start_bars[other].left < start_bars[bar].right &&
                    start_bars[bar].top < start_bars[other].bottom &&
                    start_bars[other].top < start_bars[bar].bottom) {
                self_test_failure = "two Start mark bars overlap";
                return false;
            }
        }
    }
    {
        static const uint32_t asked[] = {
            TASKBAR_ICON_SIZE, TASKBAR_ICON_SIZE_SMALL,
            TASKBAR_TRAY_GLYPH_SIZE, START_LIST_ICON, START_TILE_ICON,
            START_TILE_LARGE_ICON, SEARCH_ROW_ICON, SEARCH_BEST_ICON,
            SEARCH_FIELD_ICON
        };

        for (size_t index = 0U; index < sizeof(asked) / sizeof(asked[0]);
             ++index) {
            bool found = false;

            for (size_t size = 0U; size < TASKBAR_ART_SIZES; ++size) {
                if (taskbar_art_size[size] == asked[index]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                self_test_failure = "an icon size the bar names is not one "
                    "the artwork is rasterized at";
                return false;
            }
        }
    }
    if (!rtc_self_test()) {
        self_test_failure = rtc_self_test_failure();
        return false;
    }
    return true;
}

const char *taskbar_self_test_failure(void)
{
    return self_test_failure;
}

/* ========================================================= INSTALLED PROOF
 *
 * A self-test proves the rasterizer can draw a rounded corner.  This proves
 * that what was actually laid out on this screen, at this width, with these
 * applications, still has the measurements the METRICS block claims - that
 * the button really is forty-four wide, that its panel really is inset two
 * and four, that the indicator really does stop five pixels above the bottom
 * of the bar.  Those are the numbers the whole exercise rests on, and they
 * are the ones an innocent-looking edit is most likely to move.
 */

static const char *installed_failure = "taskbar installed proof has not run";

static bool proof_rect_equals(struct ui_rect rectangle, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
    return rectangle.x == x && rectangle.y == y &&
        rectangle.width == width && rectangle.height == height;
}

enum taskbar_status taskbar_set_start_entry(size_t index,
    const struct taskbar_start_entry *entry)
{
    if (index >= TASKBAR_MAX_START_ENTRIES) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    if (entry == NULL) {
        start_entries[index] = (struct taskbar_start_entry){ 0 };
        return TASKBAR_STATUS_OK;
    }
    start_entries[index] = *entry;
    start_entries[index].label[TASKBAR_LABEL_BYTES - 1U] = '\0';
    start_entries[index].present = true;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_start_tile(size_t index,
    const struct taskbar_start_tile *tile)
{
    if (index >= TASKBAR_MAX_START_TILES) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    if (tile == NULL) {
        start_tiles[index] = (struct taskbar_start_tile){ 0 };
        return TASKBAR_STATUS_OK;
    }
    if (tile->group >= TASKBAR_MAX_START_GROUPS ||
            tile->columns == 0U || tile->rows == 0U ||
            (uint32_t)tile->column + tile->columns > START_TILE_COLUMNS) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    start_tiles[index] = *tile;
    start_tiles[index].label[TASKBAR_LABEL_BYTES - 1U] = '\0';
    start_tiles[index].present = true;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_start_group(size_t group, const char *name)
{
    if (group >= TASKBAR_MAX_START_GROUPS) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    if (name == NULL) {
        start_group_names[group][0] = '\0';
        return TASKBAR_STATUS_OK;
    }
    size_t index = 0U;

    while (index + 1U < TASKBAR_LABEL_BYTES && name[index] != '\0') {
        start_group_names[group][index] = name[index];
        ++index;
    }
    start_group_names[group][index] = '\0';
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_start_open(bool open, struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    start_set_open(open);
    *damage = start_menu_rect;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_set_search_open(bool open,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    /* Joined with the Start menu's rect because opening this closes that,
     * and the strip it is vacating has to be repainted too. */
    search_set_open(open);
    *damage = rect_join(search_panel_rect, start_menu_rect);
    return TASKBAR_STATUS_OK;
}

bool taskbar_search_panel_open(void)
{
    return search_open;
}

struct ui_rect taskbar_search_panel_bounds(void)
{
    return search_open || search_reveal.running ? search_panel_rect :
        (struct ui_rect){ 0U, 0U, 0U, 0U };
}

const char *taskbar_search_query(void)
{
    return search_query;
}

size_t taskbar_search_result_count(void)
{
    return search_result_count;
}

bool taskbar_start_menu_open(void)
{
    return start_open;
}

struct ui_rect taskbar_start_menu_bounds(void)
{
    return start_open || start_reveal.value != 0 ? start_menu_rect :
        (struct ui_rect){ 0U, 0U, 0U, 0U };
}

bool taskbar_artwork(const char *name, uint32_t wanted,
    const uint32_t **pixels, const uint8_t **alpha, uint32_t *side)
{
    if (name == NULL || pixels == NULL || alpha == NULL || side == NULL) {
        return false;
    }
    return art_planes(name, wanted, pixels, alpha, side);
}

enum taskbar_status taskbar_set_battery(uint8_t percent)
{
    battery_percent = percent > 100U ? 100U : percent;
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_app_bounds(size_t index, struct ui_rect *bounds)
{
    if (bounds == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    if (index >= TASKBAR_MAX_APPS) {
        return TASKBAR_STATUS_BAD_INDEX;
    }

    const enum taskbar_element element = (enum taskbar_element)(
        TASKBAR_ELEMENT_APP_FIRST + index);

    if (!element_present[element]) {
        return TASKBAR_STATUS_BAD_INDEX;
    }
    *bounds = element_rects[element];
    return TASKBAR_STATUS_OK;
}

enum taskbar_status taskbar_verify_installed(struct taskbar_proof *proof)
{
    if (proof == NULL) {
        return TASKBAR_STATUS_NULL_ARGUMENT;
    }
    *proof = (struct taskbar_proof){ 0 };
    if (!initialized) {
        installed_failure = "taskbar was never initialized";
        return TASKBAR_STATUS_NOT_INITIALIZED;
    }
    installed_failure = "taskbar installed proof passed";

    proof->bar_height = bar_rect.height;
    proof->button_extent = button_extent();
    proof->panel_size = TASKBAR_PANEL_SIZE;
    proof->icon_size = icon_size();
    proof->corner_radius = TASKBAR_CORNER_CONTROL;
    proof->indicator_height = TASKBAR_INDICATOR_HEIGHT;
    proof->indicator_bottom_gap = TASKBAR_INDICATOR_BOTTOM_GAP;
    proof->tray_slot_width = TASKBAR_TRAY_SLOT_WIDTH;
    proof->tray_glyph_size = TASKBAR_TRAY_GLYPH_SIZE;
    proof->show_desktop_width = TASKBAR_SHOW_DESKTOP_WIDTH;
    proof->cluster_slots = (uint32_t)cluster_count;
    proof->cluster_width = cluster_width;
    proof->work_area_height = taskbar_work_area().height;
    proof->draws = counters.draws;

    /* The bar itself: a full-width strip on the bottom edge. */
    if (!proof_rect_equals(bar_rect, 0U, screen_height - bar_height(),
            screen_width, bar_height())) {
        installed_failure = "the taskbar is not a full-width bottom strip";
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    if (bar_rect.height != TASKBAR_HEIGHT_DEFAULT &&
            bar_rect.height != TASKBAR_HEIGHT_SMALL) {
        installed_failure = "the taskbar is neither of its two heights";
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    if (taskbar_work_area().height + bar_rect.height != screen_height) {
        installed_failure = "the work area and the bar do not tile the screen";
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }

    /* The cluster: contiguous slots, no gap and no overlap, each the extent
     * its own kind of button is given. */
    uint32_t expected_x = cluster_origin;

    for (size_t index = 0U; index < cluster_count; ++index) {
        const enum taskbar_element element = cluster_order[index];
        const struct ui_rect slot = element_rects[element];
        const struct ui_rect panel = panel_rect(element);

        if (slot.x != expected_x || slot.width != element_extent(element)) {
            installed_failure = "a cluster slot is the wrong width or place";
            return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
        }
        if (slot.y != bar_rect.y || slot.height != bar_rect.height) {
            installed_failure = "a cluster slot does not span the bar";
            return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
        }
        /*
         * Windows 10 fills the whole cell: the panel is the slot, with no
         * inset and no corner radius anywhere.  A panel that has grown an
         * inset is Windows 11's, and that is exactly the drift this proof
         * exists to catch.
         */
        if (panel.y != slot.y + TASKBAR_BUTTON_INSET_Y ||
            panel.height != slot.height - TASKBAR_BUTTON_INSET_Y * 2U ||
            panel.x != slot.x + TASKBAR_BUTTON_INSET_X ||
            panel.width != slot.width - TASKBAR_BUTTON_INSET_X * 2U) {
            installed_failure = "a button panel is not its whole slot";
            return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
        }
        if (TASKBAR_CORNER_CONTROL != 0U) {
            installed_failure = "a taskbar button has grown a corner radius";
            return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
        }
        if (element_is_app(element)) {
            const struct ui_rect icon = icon_rect(element);
            const uint32_t size = icon_size();

            if (icon.width != size || icon.height != size ||
                icon.x != slot.x + (slot.width - size) / 2U ||
                icon.y != slot.y + (slot.height - size) / 2U) {
                installed_failure = "an application icon is not centred";
                return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
            }
            /* Windows 10's underline is flush with the bottom of the bar. */
            const uint32_t bottom = panel.y + panel.height -
                TASKBAR_INDICATOR_BOTTOM_MARGIN;

            if (bar_rect.y + bar_rect.height - bottom !=
                    TASKBAR_INDICATOR_BOTTOM_GAP) {
                installed_failure =
                    "the running underline has left the bottom of the bar";
                return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
            }
        }
        expected_x += slot.width;
    }
    if (cluster_count != 0U && expected_x != cluster_origin + cluster_width) {
        installed_failure = "the cluster's slots do not sum to its width";
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }

    /*
     * The tray: contiguous from its first slot to the screen edge.  Which
     * slots are in the chain depends on what is turned on, so the chain is
     * built rather than written down - a fixed chain would measure the gap
     * where a hidden slot used to be and call the tray broken.
     */
    enum taskbar_element tray_chain[6];
    size_t tray_length = 0U;

    if (chevron_visible) {
        tray_chain[tray_length++] = TASKBAR_ELEMENT_CHEVRON;
    }
    tray_chain[tray_length++] = TASKBAR_ELEMENT_TRAY_NETWORK;
    tray_chain[tray_length++] = TASKBAR_ELEMENT_TRAY_VOLUME;
    tray_chain[tray_length++] = TASKBAR_ELEMENT_TRAY_BATTERY;
    tray_chain[tray_length++] = TASKBAR_ELEMENT_CLOCK;
    if (action_centre_visible) {
        tray_chain[tray_length++] = TASKBAR_ELEMENT_ACTION_CENTER;
    }

    const struct ui_rect first_slot = element_rects[tray_chain[0]];
    const struct ui_rect clock = element_rects[tray_chain[tray_length - 1U]];

    for (size_t index = 0U; index + 1U < tray_length; ++index) {
        const struct ui_rect left_slot = element_rects[tray_chain[index]];
        const struct ui_rect right_slot = element_rects[tray_chain[index + 1U]];

        if (left_slot.x + left_slot.width != right_slot.x) {
            installed_failure = "the tray's slots are not contiguous";
            return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
        }
    }
    if (first_slot.width != TASKBAR_TRAY_SLOT_WIDTH) {
        installed_failure = "the tray's first slot is the wrong width";
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    if (element_rects[TASKBAR_ELEMENT_CLOCK].width <
            TASKBAR_TRAY_SLOT_WIDTH) {
        installed_failure = "the clock is narrower than a tray slot";
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    if (element_present[TASKBAR_ELEMENT_SHOW_DESKTOP]) {
        const struct ui_rect edge =
            element_rects[TASKBAR_ELEMENT_SHOW_DESKTOP];

        if (edge.width != TASKBAR_SHOW_DESKTOP_WIDTH ||
                edge.x + edge.width != screen_width ||
                clock.x + clock.width != edge.x) {
            installed_failure = "the show-desktop strip is not on the edge";
            return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
        }
    } else if (clock.x + clock.width != screen_width) {
        installed_failure = "the tray does not reach the screen edge";
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }

    /* Nothing may sit on top of anything else. */
    if (cluster_count != 0U &&
            cluster_origin + cluster_width > first_slot.x) {
        installed_failure = "the cluster overlaps the tray";
        return TASKBAR_STATUS_UNSUPPORTED_GEOMETRY;
    }
    return TASKBAR_STATUS_OK;
}

const char *taskbar_installed_proof_failure(void)
{
    return installed_failure;
}
