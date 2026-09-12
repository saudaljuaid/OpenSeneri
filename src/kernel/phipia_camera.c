/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Camera.  See include/phipia/camera.h for the shape, and for the two
 * busier versions of this window that were thrown away to get here.
 *
 * Top bar, frame, bottom bar with the shutter in the middle.  The motion
 * comes from ui_motion.h.
 */

#include <phipia/phipia_camera.h>

#include <phipia/clock.h>
#include <phipia/framebuffer.h>
#include <phipia/ui_font.h>

#include "phipia_camera_glyphs.h"
#include "ui_motion.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION: taken off a camera window at the size it was shown
 * at rather than measured at 100% scaling.  What is certain is the
 * ARRANGEMENT - timer left, modes centred, menu and close right, the
 * shutter centred at the bottom with the round thumbnail beside it - and
 * the four-to-three frame.
 *
 * The glyph sizes are sizes the rasterizer cut natively, and every box that
 * holds one is an even number of pixels bigger than it, so the centring
 * comes out whole.  A hinted stroke landed on a half pixel is a smeared
 * stroke.
 */

#define PHIPIA_CAMERA_BORDER 1U
#define PHIPIA_CAMERA_TOP_BAR 44U
#define PHIPIA_CAMERA_BOTTOM_BAR 96U
#define PHIPIA_CAMERA_GLYPH 24U
/* The timer, and the menu and close marks, inset from the bar's ends. */
#define PHIPIA_CAMERA_BAR_INSET 16U
#define PHIPIA_CAMERA_BAR_BUTTON 44U
#define PHIPIA_CAMERA_MENU_WIDTH 18U
#define PHIPIA_CAMERA_MENU_GAP 5U
#define PHIPIA_CAMERA_CLOSE_MARK 11U
/* The photo/video control in the middle of the top bar: two rounded
 * segments side by side with a hair between them. */
#define PHIPIA_CAMERA_SEGMENT_WIDTH 50U
#define PHIPIA_CAMERA_SEGMENT_HEIGHT 30U
#define PHIPIA_CAMERA_SEGMENT_GAP 2U
#define PHIPIA_CAMERA_SEGMENT_RADIUS 4U
/* The shutter: a filled disc, a gap, and a thin ring outside it. */
#define PHIPIA_CAMERA_SHUTTER 62U
#define PHIPIA_CAMERA_SHUTTER_CORE 25U         /* the filled disc's radius */
/* The last capture, which is round rather than square. */
#define PHIPIA_CAMERA_THUMB 42U
/* How far left of the shutter it sits, centre to centre. */
#define PHIPIA_CAMERA_THUMB_OFFSET 108U

#define PHIPIA_CAMERA_FLASH_ALPHA 210U
#define PHIPIA_CAMERA_RING_ALPHA 230U

/* ================================================================ PALETTE
 *
 * Grey and white.  A camera window is a picture with a few marks around it;
 * a second colour in the chrome would be competing with the picture.
 */

struct phipia_camera_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define PHIPIA_CAMERA_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct phipia_camera_rgb top_bar = PHIPIA_CAMERA_RGB(0x1CU, 0x1CU, 0x1CU);
/* The letterbox above and below the frame, and the bar the shutter is on. */
static const struct phipia_camera_rgb surround = PHIPIA_CAMERA_RGB(0x00U, 0x00U, 0x00U);
/* The frame with nothing behind it.  Phipia has no camera driver. */
static const struct phipia_camera_rgb empty_frame = PHIPIA_CAMERA_RGB(0x3AU, 0x3AU, 0x3AU);
static const struct phipia_camera_rgb segment_rest = PHIPIA_CAMERA_RGB(0x44U, 0x44U,
    0x44U);
static const struct phipia_camera_rgb segment_on = PHIPIA_CAMERA_RGB(0x6AU, 0x6AU, 0x6AU);
static const struct phipia_camera_rgb ink = PHIPIA_CAMERA_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct phipia_camera_rgb ink_dim = PHIPIA_CAMERA_RGB(0x8CU, 0x8CU, 0x8CU);
static const struct phipia_camera_rgb accent = PHIPIA_CAMERA_RGB(0x00U, 0x78U, 0xD7U);
static const struct phipia_camera_rgb border_inactive = PHIPIA_CAMERA_RGB(0x9BU, 0x9BU,
    0x9BU);

/* ================================================================ CONTROLS */

enum phipia_camera_control {
    PHIPIA_CAMERA_CONTROL_TIMER = 0,
    PHIPIA_CAMERA_CONTROL_PHOTO,
    PHIPIA_CAMERA_CONTROL_VIDEO,
    PHIPIA_CAMERA_CONTROL_MENU,
    PHIPIA_CAMERA_CONTROL_CLOSE,
    PHIPIA_CAMERA_CONTROL_SHUTTER,
    PHIPIA_CAMERA_CONTROL_THUMB,
    PHIPIA_CAMERA_CONTROL_COUNT
};

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool focused = true;
static bool feed_present;
static enum phipia_camera_mode current_mode = PHIPIA_CAMERA_MODE_PHOTO;
static struct ui_motion hover[PHIPIA_CAMERA_CONTROL_COUNT];
/* Nought at photo, one at video: the lit segment travels rather than
 * jumping, which is the difference between a control and a repaint. */
static struct ui_motion mode_shift;
static struct ui_motion shutter;
static bool shutter_running;

/* The last capture, sampled off the frame so the thumbnail is the picture
 * that was taken rather than a stand-in for one. */
static uint32_t thumb_pixels[PHIPIA_CAMERA_THUMB * PHIPIA_CAMERA_THUMB];
static bool thumb_valid;

static const char *self_test_failure = "camera self-test has not run";

const char *phipia_camera_status_string(enum phipia_camera_status status)
{
    switch (status) {
    case PHIPIA_CAMERA_STATUS_OK:
        return "ok";
    case PHIPIA_CAMERA_STATUS_NULL_ARGUMENT:
        return "null argument";
    case PHIPIA_CAMERA_STATUS_NOT_INITIALIZED:
        return "camera not initialized";
    case PHIPIA_CAMERA_STATUS_BAD_INDEX:
        return "camera index is out of range";
    case PHIPIA_CAMERA_STATUS_UNSUPPORTED_GEOMETRY:
        return "camera geometry is unsupported";
    case PHIPIA_CAMERA_STATUS_SURFACE_FAILURE:
        return "camera surface refused a pixel";
    default:
        return "unknown camera status";
    }
}

/* ============================================================== PRIMITIVES */

static uint32_t pack_rgb(struct phipia_camera_rgb colour)
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

static struct ui_rect rect_join(struct ui_rect left, struct ui_rect right)
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

static struct ui_rect inflate(struct ui_rect area, uint32_t by)
{
    if (area.width == 0U || area.height == 0U) {
        return area;
    }
    return (struct ui_rect){
        area.x > by ? area.x - by : 0U,
        area.y > by ? area.y - by : 0U,
        area.width + by * 2U, area.height + by * 2U };
}

static bool holds(struct ui_rect area, struct ui_point point)
{
    return point.x >= 0 && point.y >= 0 &&
        (uint32_t)point.x >= area.x &&
        (uint32_t)point.x < area.x + area.width &&
        (uint32_t)point.y >= area.y &&
        (uint32_t)point.y < area.y + area.height;
}

static enum phipia_camera_status fill(struct ui_rect area, struct ui_rect damage,
    struct phipia_camera_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return PHIPIA_CAMERA_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return PHIPIA_CAMERA_STATUS_OK;
}

static enum phipia_camera_status outline(struct ui_rect area,
    struct ui_rect damage, struct phipia_camera_rgb colour)
{
    enum phipia_camera_status status = fill((struct ui_rect){ area.x, area.y,
        area.width, PHIPIA_CAMERA_BORDER }, damage, colour);

    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = fill((struct ui_rect){ area.x,
            area.y + area.height - PHIPIA_CAMERA_BORDER, area.width,
            PHIPIA_CAMERA_BORDER }, damage, colour);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y, PHIPIA_CAMERA_BORDER,
            area.height }, damage, colour);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = fill((struct ui_rect){ area.x + area.width - PHIPIA_CAMERA_BORDER,
            area.y, PHIPIA_CAMERA_BORDER, area.height }, damage, colour);
    }
    return status;
}

/* One pixel mixed into what is already there. */
static enum phipia_camera_status blend_pixel(struct framebuffer_state format,
    uint32_t x, uint32_t y, uint32_t over, uint32_t alpha)
{
    uint32_t under;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    if (alpha == 0U) {
        return PHIPIA_CAMERA_STATUS_OK;
    }
    if (surface_read_pixel(canvas, x, y, &under) != SURFACE_STATUS_OK) {
        return PHIPIA_CAMERA_STATUS_SURFACE_FAILURE;
    }
    red = (((over >> format.red_position) & 0xFFU) * alpha +
        ((under >> format.red_position) & 0xFFU) * (255U - alpha) + 127U) /
        255U;
    green = (((over >> format.green_position) & 0xFFU) * alpha +
        ((under >> format.green_position) & 0xFFU) * (255U - alpha) + 127U) /
        255U;
    blue = (((over >> format.blue_position) & 0xFFU) * alpha +
        ((under >> format.blue_position) & 0xFFU) * (255U - alpha) + 127U) /
        255U;
    if (surface_pixel(canvas, x, y, (red << format.red_position) |
            (green << format.green_position) |
            (blue << format.blue_position)) != SURFACE_STATUS_OK) {
        return PHIPIA_CAMERA_STATUS_SURFACE_FAILURE;
    }
    return PHIPIA_CAMERA_STATUS_OK;
}

/* A rectangle mixed in, which is what the flash needs: it has to wash the
 * frame, not replace it. */
static enum phipia_camera_status blend(struct ui_rect area, struct ui_rect damage,
    struct phipia_camera_rgb colour, uint32_t alpha)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t over = pack_rgb(colour);

    if (alpha == 0U) {
        return PHIPIA_CAMERA_STATUS_OK;
    }
    if (alpha >= 255U) {
        return fill(area, damage, colour);
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const enum phipia_camera_status status = blend_pixel(format,
                clipped.x + x, clipped.y + y, over, alpha);

            if (status != PHIPIA_CAMERA_STATUS_OK) {
                return status;
            }
        }
    }
    return PHIPIA_CAMERA_STATUS_OK;
}

/*
 * An integer square root, for the distance a round edge is measured by.
 *
 * The kernel builds with no floating point at all, so the alternative is
 * comparing squared distances - which gives a hard edge, and a hard edge on
 * a sixty-two pixel shutter is a staircase.
 */
static uint32_t isqrt64(uint64_t value)
{
    uint64_t root = 0U;
    uint64_t bit = UINT64_C(1) << 62;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0U) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

/* Coverage of one pixel by an edge that far away, in 256ths of a pixel. */
static uint32_t edge_coverage(int64_t distance_q8)
{
    const int64_t inside = 128 - distance_q8;

    if (inside <= -128) {
        return 0U;
    }
    if (inside >= 128) {
        return 255U;
    }
    return (uint32_t)((inside + 128) * 255 / 256);
}

/*
 * A disc, and a ring: the same walk with the distance measured from the
 * radius rather than from the centre, so the two are one function with a
 * width.  A width of zero means filled.  Radius and width are in 256ths.
 */
static enum phipia_camera_status blend_round(struct ui_rect box, struct ui_rect
    damage, struct phipia_camera_rgb colour, uint32_t alpha, uint32_t radius_q8,
    uint32_t width_q8)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const struct ui_rect clipped = intersect(box, damage);
    const uint32_t over = pack_rgb(colour);
    const int64_t centre_x = (int64_t)box.x * 256 + (int64_t)box.width * 128;
    const int64_t centre_y = (int64_t)box.y * 256 + (int64_t)box.height * 128;

    if (alpha == 0U) {
        return PHIPIA_CAMERA_STATUS_OK;
    }
    if (alpha > 255U) {
        alpha = 255U;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const int64_t dy = (int64_t)(clipped.y + y) * 256 + 128 - centre_y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const int64_t dx = (int64_t)(clipped.x + x) * 256 + 128 -
                centre_x;
            const uint32_t distance = isqrt64((uint64_t)(dx * dx + dy * dy));
            int64_t outside;
            uint32_t coverage;
            enum phipia_camera_status status;

            if (width_q8 == 0U) {
                outside = (int64_t)distance - (int64_t)radius_q8;
            } else {
                const int64_t from_ring = (int64_t)distance -
                    (int64_t)radius_q8;

                outside = (from_ring < 0 ? -from_ring : from_ring) -
                    (int64_t)width_q8;
            }
            coverage = edge_coverage(outside);
            if (coverage == 0U) {
                continue;
            }
            status = blend_pixel(format, clipped.x + x, clipped.y + y, over,
                (coverage * alpha + 127U) / 255U);
            if (status != PHIPIA_CAMERA_STATUS_OK) {
                return status;
            }
        }
    }
    return PHIPIA_CAMERA_STATUS_OK;
}

/*
 * A rounded rectangle, for the two mode segments.
 *
 * The distance to a rounded rectangle is the distance to the rectangle it
 * would be if its corners were pulled in by the radius - nought anywhere
 * inside that - so the corners come out as arcs from the same edge function
 * the discs use, and the straight sides stay exactly straight.
 */
static enum phipia_camera_status blend_rounded(struct ui_rect box,
    struct ui_rect damage, struct phipia_camera_rgb colour, uint32_t alpha,
    uint32_t radius)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const struct ui_rect clipped = intersect(box, damage);
    const uint32_t over = pack_rgb(colour);
    const int64_t radius_q8 = (int64_t)radius * 256;
    const int64_t left = (int64_t)box.x * 256 + radius_q8;
    const int64_t right = (int64_t)(box.x + box.width) * 256 - radius_q8;
    const int64_t top = (int64_t)box.y * 256 + radius_q8;
    const int64_t bottom = (int64_t)(box.y + box.height) * 256 - radius_q8;

    if (alpha == 0U) {
        return PHIPIA_CAMERA_STATUS_OK;
    }
    if (alpha > 255U) {
        alpha = 255U;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const int64_t py = (int64_t)(clipped.y + y) * 256 + 128;
        const int64_t dy = py < top ? top - py : (py > bottom ? py - bottom :
            0);

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const int64_t px = (int64_t)(clipped.x + x) * 256 + 128;
            const int64_t dx = px < left ? left - px :
                (px > right ? px - right : 0);
            const uint32_t distance = isqrt64((uint64_t)(dx * dx + dy * dy));
            const uint32_t coverage = edge_coverage((int64_t)distance -
                radius_q8);
            enum phipia_camera_status status;

            if (coverage == 0U) {
                continue;
            }
            status = blend_pixel(format, clipped.x + x, clipped.y + y, over,
                (coverage * alpha + 127U) / 255U);
            if (status != PHIPIA_CAMERA_STATUS_OK) {
                return status;
            }
        }
    }
    return PHIPIA_CAMERA_STATUS_OK;
}

static bool names_match(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

/*
 * The cell cut at exactly this size, or nothing.
 *
 * Not "the largest that fits": every size this window asks for is a size
 * the rasterizer was told to cut, so a miss is a mistake in the metrics
 * rather than something to paper over with a smaller mark.  The self-test
 * checks every one of them.
 */
static const uint8_t *glyph_cell(const char *name, uint32_t wanted)
{
    for (size_t index = 0U; index < PHIPIA_CAMERA_LUCIDE_COUNT; ++index) {
        if (name == NULL || !names_match(phipia_camera_lucide[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < PHIPIA_CAMERA_LUCIDE_SIZES; ++option) {
            if (phipia_camera_lucide_size[option] == wanted) {
                return phipia_camera_lucide[index].alpha[option];
            }
        }
        return NULL;
    }
    return NULL;
}

/*
 * A mark centred in a box.
 *
 * Composited one pixel to one pixel and never resampled, so the box has to
 * be at least as big as the cell and the centring has to come out whole.
 * That is what keeps a hinted stroke on the pixel it was hinted onto.
 */
static enum phipia_camera_status draw_glyph(const char *name, uint32_t size,
    struct ui_rect box, struct ui_rect damage, struct phipia_camera_rgb colour,
    uint32_t opacity)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t over = pack_rgb(colour);
    const uint8_t *cell = glyph_cell(name, size);
    struct ui_rect placed;
    struct ui_rect clipped;

    if (cell == NULL || opacity == 0U || box.width < size ||
            box.height < size) {
        return PHIPIA_CAMERA_STATUS_OK;
    }
    if (opacity > 255U) {
        opacity = 255U;
    }
    placed = (struct ui_rect){ box.x + (box.width - size) / 2U,
        box.y + (box.height - size) / 2U, size, size };
    clipped = intersect(placed, damage);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint8_t coverage = cell[local_y * size +
                (clipped.x - placed.x + x)];
            const enum phipia_camera_status status = blend_pixel(format,
                clipped.x + x, clipped.y + y, over,
                ((uint32_t)coverage * opacity + 127U) / 255U);

            if (status != PHIPIA_CAMERA_STATUS_OK) {
                return status;
            }
        }
    }
    return PHIPIA_CAMERA_STATUS_OK;
}

/* ================================================================ GEOMETRY */

static struct ui_rect inner_rect(void)
{
    return (struct ui_rect){ window_rect.x + PHIPIA_CAMERA_BORDER,
        window_rect.y + PHIPIA_CAMERA_BORDER,
        window_rect.width - PHIPIA_CAMERA_BORDER * 2U,
        window_rect.height - PHIPIA_CAMERA_BORDER * 2U };
}

static struct ui_rect top_bar_rect(void)
{
    const struct ui_rect inner = inner_rect();

    return (struct ui_rect){ inner.x, inner.y, inner.width, PHIPIA_CAMERA_TOP_BAR };
}

static struct ui_rect bottom_bar_rect(void)
{
    const struct ui_rect inner = inner_rect();

    return (struct ui_rect){ inner.x,
        inner.y + inner.height - PHIPIA_CAMERA_BOTTOM_BAR, inner.width,
        PHIPIA_CAMERA_BOTTOM_BAR };
}

/* Between the bars: the frame and its letterbox. */
static struct ui_rect stage_rect(void)
{
    const struct ui_rect inner = inner_rect();
    const uint32_t bars = PHIPIA_CAMERA_TOP_BAR + PHIPIA_CAMERA_BOTTOM_BAR;

    return (struct ui_rect){ inner.x, inner.y + PHIPIA_CAMERA_TOP_BAR, inner.width,
        inner.height > bars ? inner.height - bars : 0U };
}

/*
 * The frame: four to three, as wide as the window lets it be, letterboxed
 * above and below.  Full width is the reference's - the picture runs edge to
 * edge and the black is only ever top and bottom.
 */
struct ui_rect phipia_camera_viewfinder_bounds(void)
{
    const struct ui_rect stage = stage_rect();
    uint32_t width = stage.width;
    uint32_t height;

    if (stage.width == 0U || stage.height == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    height = width * 3U / 4U;
    if (height > stage.height) {
        height = stage.height;
        width = height * 4U / 3U;
    }
    return (struct ui_rect){ stage.x + (stage.width - width) / 2U,
        stage.y + (stage.height - height) / 2U, width, height };
}

struct ui_rect phipia_camera_capture_bounds(void)
{
    const struct ui_rect bar = bottom_bar_rect();

    return (struct ui_rect){
        bar.x + (bar.width - PHIPIA_CAMERA_SHUTTER) / 2U,
        bar.y + (bar.height - PHIPIA_CAMERA_SHUTTER) / 2U,
        PHIPIA_CAMERA_SHUTTER, PHIPIA_CAMERA_SHUTTER };
}

static struct ui_rect control_rect(enum phipia_camera_control control)
{
    const struct ui_rect bar = top_bar_rect();
    const uint32_t middle = bar.y + (PHIPIA_CAMERA_TOP_BAR - PHIPIA_CAMERA_GLYPH) / 2U;
    const uint32_t group = PHIPIA_CAMERA_SEGMENT_WIDTH * 2U + PHIPIA_CAMERA_SEGMENT_GAP;
    const uint32_t group_left = bar.x + (bar.width - group) / 2U;
    const uint32_t segment_top = bar.y +
        (PHIPIA_CAMERA_TOP_BAR - PHIPIA_CAMERA_SEGMENT_HEIGHT) / 2U;
    const struct ui_rect shutter_box = phipia_camera_capture_bounds();

    switch (control) {
    case PHIPIA_CAMERA_CONTROL_TIMER:
        return (struct ui_rect){ bar.x + PHIPIA_CAMERA_BAR_INSET, middle,
            PHIPIA_CAMERA_GLYPH, PHIPIA_CAMERA_GLYPH };
    case PHIPIA_CAMERA_CONTROL_PHOTO:
        return (struct ui_rect){ group_left, segment_top,
            PHIPIA_CAMERA_SEGMENT_WIDTH, PHIPIA_CAMERA_SEGMENT_HEIGHT };
    case PHIPIA_CAMERA_CONTROL_VIDEO:
        return (struct ui_rect){
            group_left + PHIPIA_CAMERA_SEGMENT_WIDTH + PHIPIA_CAMERA_SEGMENT_GAP,
            segment_top, PHIPIA_CAMERA_SEGMENT_WIDTH, PHIPIA_CAMERA_SEGMENT_HEIGHT };
    case PHIPIA_CAMERA_CONTROL_MENU:
        return (struct ui_rect){
            bar.x + bar.width - PHIPIA_CAMERA_BAR_BUTTON * 2U, bar.y,
            PHIPIA_CAMERA_BAR_BUTTON, PHIPIA_CAMERA_TOP_BAR };
    case PHIPIA_CAMERA_CONTROL_CLOSE:
        return (struct ui_rect){ bar.x + bar.width - PHIPIA_CAMERA_BAR_BUTTON,
            bar.y, PHIPIA_CAMERA_BAR_BUTTON, PHIPIA_CAMERA_TOP_BAR };
    case PHIPIA_CAMERA_CONTROL_SHUTTER:
        return shutter_box;
    case PHIPIA_CAMERA_CONTROL_THUMB:
        return (struct ui_rect){
            shutter_box.x + PHIPIA_CAMERA_SHUTTER / 2U - PHIPIA_CAMERA_THUMB_OFFSET -
                PHIPIA_CAMERA_THUMB / 2U,
            shutter_box.y + (PHIPIA_CAMERA_SHUTTER - PHIPIA_CAMERA_THUMB) / 2U,
            PHIPIA_CAMERA_THUMB, PHIPIA_CAMERA_THUMB };
    default:
        break;
    }
    return (struct ui_rect){ 0U, 0U, 0U, 0U };
}

struct ui_rect phipia_camera_bounds(void)
{
    return window_rect;
}

/* ================================================================== PIECES */

/* The hamburger: three whole-pixel rules.  Drawn rather than rasterized,
 * because three straight lines snapped to the grid are sharper than any
 * cell of the same mark. */
static enum phipia_camera_status draw_menu(struct ui_rect damage)
{
    const struct ui_rect box = control_rect(PHIPIA_CAMERA_CONTROL_MENU);
    const uint32_t left = box.x + (box.width - PHIPIA_CAMERA_MENU_WIDTH) / 2U;
    const uint32_t top = box.y + (box.height - PHIPIA_CAMERA_MENU_GAP * 2U - 3U) /
        2U;
    const struct phipia_camera_rgb mark = focused ? ink : ink_dim;
    enum phipia_camera_status status = PHIPIA_CAMERA_STATUS_OK;

    for (uint32_t line = 0U; line < 3U && status == PHIPIA_CAMERA_STATUS_OK;
         ++line) {
        status = fill((struct ui_rect){ left, top + line * PHIPIA_CAMERA_MENU_GAP,
            PHIPIA_CAMERA_MENU_WIDTH, 1U }, damage, mark);
    }
    return status;
}

/* The close mark, as two one-pixel diagonals. */
static enum phipia_camera_status draw_close(struct ui_rect damage)
{
    const struct ui_rect box = control_rect(PHIPIA_CAMERA_CONTROL_CLOSE);
    const uint32_t left = box.x + (box.width - PHIPIA_CAMERA_CLOSE_MARK) / 2U;
    const uint32_t top = box.y + (box.height - PHIPIA_CAMERA_CLOSE_MARK) / 2U;
    const struct phipia_camera_rgb mark = focused ? ink : ink_dim;
    enum phipia_camera_status status = PHIPIA_CAMERA_STATUS_OK;

    for (uint32_t step = 0U; step < PHIPIA_CAMERA_CLOSE_MARK &&
            status == PHIPIA_CAMERA_STATUS_OK; ++step) {
        status = fill((struct ui_rect){ left + step, top + step, 1U, 1U },
            damage, mark);
        if (status == PHIPIA_CAMERA_STATUS_OK) {
            status = fill((struct ui_rect){ left + step,
                top + PHIPIA_CAMERA_CLOSE_MARK - 1U - step, 1U, 1U }, damage, mark);
        }
    }
    return status;
}

/*
 * The two mode segments.
 *
 * Both are drawn every frame and the lit fill travels between them, so
 * switching mode is one animation rather than two.
 */
static enum phipia_camera_status draw_segments(struct ui_rect damage)
{
    const int32_t video = mode_shift.value;
    const int32_t photo = (int32_t)UI_MOTION_ONE - mode_shift.value;
    const struct ui_rect boxes[2] = {
        control_rect(PHIPIA_CAMERA_CONTROL_PHOTO),
        control_rect(PHIPIA_CAMERA_CONTROL_VIDEO)
    };
    static const char *const marks[2] = { "camera", "video" };
    const int32_t weights[2] = { photo, video };
    enum phipia_camera_status status = PHIPIA_CAMERA_STATUS_OK;

    for (size_t index = 0U; index < 2U && status == PHIPIA_CAMERA_STATUS_OK;
         ++index) {
        const uint32_t lit = (uint32_t)((int64_t)255 * weights[index] /
            UI_MOTION_ONE);

        status = blend_rounded(boxes[index], damage, segment_rest, 255U,
            PHIPIA_CAMERA_SEGMENT_RADIUS);
        if (status == PHIPIA_CAMERA_STATUS_OK && lit > 0U) {
            status = blend_rounded(boxes[index], damage, segment_on, lit,
                PHIPIA_CAMERA_SEGMENT_RADIUS);
        }
        if (status == PHIPIA_CAMERA_STATUS_OK) {
            status = draw_glyph(marks[index], PHIPIA_CAMERA_GLYPH, boxes[index],
                damage, ink, 255U);
        }
    }
    return status;
}

static enum phipia_camera_status draw_top_bar(struct ui_rect damage)
{
    enum phipia_camera_status status = fill(top_bar_rect(), damage, top_bar);

    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_glyph("timer", PHIPIA_CAMERA_GLYPH,
            control_rect(PHIPIA_CAMERA_CONTROL_TIMER), damage,
            focused ? ink : ink_dim, 255U);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_segments(damage);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_menu(damage);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_close(damage);
    }
    return status;
}

/* The letterbox, drawn without touching the frame itself. */
static enum phipia_camera_status draw_stage(struct ui_rect damage)
{
    const struct ui_rect stage = stage_rect();
    const struct ui_rect view = phipia_camera_viewfinder_bounds();
    enum phipia_camera_status status;

    if (view.width == 0U) {
        return fill(stage, damage, surround);
    }
    status = fill((struct ui_rect){ stage.x, stage.y, stage.width,
        view.y - stage.y }, damage, surround);
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = fill((struct ui_rect){ stage.x, view.y + view.height,
            stage.width, stage.y + stage.height - view.y - view.height },
            damage, surround);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK && view.x > stage.x) {
        status = fill((struct ui_rect){ stage.x, view.y, view.x - stage.x,
            view.height }, damage, surround);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK &&
            view.x + view.width < stage.x + stage.width) {
        status = fill((struct ui_rect){ view.x + view.width, view.y,
            stage.x + stage.width - view.x - view.width, view.height },
            damage, surround);
    }
    /* Without a feed the frame is this window's to fill; with one it belongs
     * to the capture pipeline and is left alone. */
    if (status == PHIPIA_CAMERA_STATUS_OK && !feed_present) {
        status = fill(view, damage, empty_frame);
    }
    return status;
}

/* The shutter's white, brightest the instant it fires. */
static enum phipia_camera_status draw_flash(struct ui_rect damage)
{
    const struct ui_rect view = phipia_camera_viewfinder_bounds();
    const int64_t left = UI_MOTION_ONE - shutter.value;
    const uint32_t alpha = shutter_running ?
        (uint32_t)((int64_t)PHIPIA_CAMERA_FLASH_ALPHA * left / UI_MOTION_ONE) : 0U;

    if (alpha == 0U || view.width == 0U) {
        return PHIPIA_CAMERA_STATUS_OK;
    }
    return blend(view, damage, ink, alpha);
}

/*
 * The shutter: a filled disc with a thin ring outside it and a gap between.
 * No mark on it - the camera icon is up in the mode control, and a shutter
 * with an icon on it is a button pretending to be two things.
 */
static enum phipia_camera_status draw_shutter(struct ui_rect damage)
{
    const struct ui_rect box = phipia_camera_capture_bounds();
    const uint32_t lift = ui_motion_alpha(&hover[PHIPIA_CAMERA_CONTROL_SHUTTER]);
    /* Pressing is what the closing ring is for; hovering only brightens the
     * rim.  A pixel and a half wide, which is what reads as a drawn ring
     * rather than as a hairline. */
    const uint32_t rim = 240U + 15U * lift / 255U;
    enum phipia_camera_status status = blend_round(box, damage, ink, rim,
        PHIPIA_CAMERA_SHUTTER * 128U - 192U, 192U);

    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = blend_round(box, damage, ink, 255U,
            PHIPIA_CAMERA_SHUTTER_CORE * 256U, 0U);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK && shutter_running) {
        const int64_t travel = shutter.value;
        const uint32_t outer = PHIPIA_CAMERA_SHUTTER * 128U + 18U * 256U;
        const uint32_t inner = PHIPIA_CAMERA_SHUTTER * 128U + 3U * 256U;
        const uint32_t radius = outer -
            (uint32_t)((int64_t)(outer - inner) * travel / UI_MOTION_ONE);
        const uint32_t ring = (uint32_t)((int64_t)PHIPIA_CAMERA_RING_ALPHA *
            (UI_MOTION_ONE - travel) / UI_MOTION_ONE);

        status = blend_round(inflate(box, 24U), damage, ink, ring, radius,
            256U);
    }
    return status;
}

/*
 * The last capture: round, because the reference's is, and masked with the
 * same edge function the shutter uses rather than a hard circle.
 */
static enum phipia_camera_status draw_thumbnail(struct ui_rect damage)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const struct ui_rect box = control_rect(PHIPIA_CAMERA_CONTROL_THUMB);
    const struct ui_rect clipped = intersect(box, damage);
    const uint32_t lift = ui_motion_alpha(&hover[PHIPIA_CAMERA_CONTROL_THUMB]);
    const int64_t centre_x = (int64_t)box.x * 256 + (int64_t)box.width * 128;
    const int64_t centre_y = (int64_t)box.y * 256 + (int64_t)box.height * 128;
    const int64_t radius_q8 = (int64_t)PHIPIA_CAMERA_THUMB * 128;

    if (!thumb_valid) {
        /* Nothing taken yet: the ring it will appear in, and no more. */
        return blend_round(box, damage, ink_dim, 190U + lift / 4U,
            (uint32_t)radius_q8 - 192U, 192U);
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const int64_t dy = (int64_t)(clipped.y + y) * 256 + 128 - centre_y;
        const uint32_t local_y = clipped.y - box.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const int64_t dx = (int64_t)(clipped.x + x) * 256 + 128 -
                centre_x;
            const uint32_t distance = isqrt64((uint64_t)(dx * dx + dy * dy));
            const uint32_t coverage = edge_coverage((int64_t)distance -
                radius_q8);
            const uint32_t local_x = clipped.x - box.x + x;
            enum phipia_camera_status status;

            if (coverage == 0U) {
                continue;
            }
            status = blend_pixel(format, clipped.x + x, clipped.y + y,
                thumb_pixels[local_y * PHIPIA_CAMERA_THUMB + local_x], coverage);
            if (status != PHIPIA_CAMERA_STATUS_OK) {
                return status;
            }
        }
    }
    /* A hairline rim, brighter under the pointer, so a dark photograph is
     * still visibly a control. */
    return blend_round(box, damage, ink, 120U + lift / 2U,
        (uint32_t)radius_q8 - 128U, 128U);
}

enum phipia_camera_status phipia_camera_draw(struct ui_rect damage)
{
    enum phipia_camera_status status;

    if (!initialized) {
        return PHIPIA_CAMERA_STATUS_NOT_INITIALIZED;
    }
    /*
     * The border is four edges rather than a fill under everything else:
     * with a feed running, the frame's pixels belong to the capture
     * pipeline, and a window that painted over them first would show its
     * own accent for a frame - or, if nothing redrew the feed, for good.
     */
    status = outline(window_rect, damage,
        focused ? accent : border_inactive);
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_top_bar(damage);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_stage(damage);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_flash(damage);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = fill(bottom_bar_rect(), damage, surround);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_shutter(damage);
    }
    if (status == PHIPIA_CAMERA_STATUS_OK) {
        status = draw_thumbnail(damage);
    }
    return status;
}

/* ================================================================== INPUT */

static struct ui_rect control_damage(enum phipia_camera_control control)
{
    /* Wide enough for the shutter's ring, which draws outside its button. */
    return inflate(control_rect(control), 26U);
}

enum phipia_camera_status phipia_camera_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const uint64_t now = clock_monotonic_ns();

    if (damage == NULL) {
        return PHIPIA_CAMERA_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PHIPIA_CAMERA_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    for (int control = 0; control < PHIPIA_CAMERA_CONTROL_COUNT; ++control) {
        const bool over = holds(control_rect((enum phipia_camera_control)control),
            point);
        const int32_t target = over ? (int32_t)UI_MOTION_ONE : 0;

        if (hover[control].target == target) {
            continue;
        }
        /* 83 ms, linear: a XAML BrushTransition, the same cross-fade the
         * taskbar's buttons use.  See ui_motion.h. */
        ui_motion_to(&hover[control], target, UI_MOTION_BRUSH_NS, now);
        *damage = rect_join(*damage,
            control_damage((enum phipia_camera_control)control));
    }
    return PHIPIA_CAMERA_STATUS_OK;
}

enum phipia_camera_status phipia_camera_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return PHIPIA_CAMERA_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PHIPIA_CAMERA_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (holds(phipia_camera_capture_bounds(), point)) {
        return phipia_camera_capture(damage);
    }
    if (holds(control_rect(PHIPIA_CAMERA_CONTROL_PHOTO), point)) {
        (void)phipia_camera_set_mode(PHIPIA_CAMERA_MODE_PHOTO);
        *damage = top_bar_rect();
        return PHIPIA_CAMERA_STATUS_OK;
    }
    if (holds(control_rect(PHIPIA_CAMERA_CONTROL_VIDEO), point)) {
        (void)phipia_camera_set_mode(PHIPIA_CAMERA_MODE_VIDEO);
        *damage = top_bar_rect();
        return PHIPIA_CAMERA_STATUS_OK;
    }
    /* The timer, the menu, the close mark and the thumbnail all open things
     * this window does not own yet, so they take the press and report no
     * damage rather than pretending to have acted. */
    return PHIPIA_CAMERA_STATUS_OK;
}

enum phipia_camera_status phipia_camera_capture(struct ui_rect *damage)
{
    const struct ui_rect view = phipia_camera_viewfinder_bounds();

    if (damage == NULL) {
        return PHIPIA_CAMERA_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PHIPIA_CAMERA_STATUS_NOT_INITIALIZED;
    }
    /*
     * Sample the frame BEFORE the flash goes over it - a thumbnail taken
     * afterwards would be a white circle, which is a funny bug to ship.
     * A square crop out of the middle rather than the whole frame squeezed
     * into the thumbnail: wrong aspect is not a small copy of the
     * photograph, it is a different picture.
     */
    if (feed_present && view.width >= PHIPIA_CAMERA_THUMB &&
            view.height >= PHIPIA_CAMERA_THUMB) {
        const uint32_t side = view.width < view.height ? view.width :
            view.height;
        const uint32_t left = view.x + (view.width - side) / 2U;
        const uint32_t top = view.y + (view.height - side) / 2U;

        for (uint32_t y = 0U; y < PHIPIA_CAMERA_THUMB; ++y) {
            for (uint32_t x = 0U; x < PHIPIA_CAMERA_THUMB; ++x) {
                uint32_t pixel = 0U;

                if (surface_read_pixel(canvas,
                        left + x * side / PHIPIA_CAMERA_THUMB,
                        top + y * side / PHIPIA_CAMERA_THUMB, &pixel) !=
                            SURFACE_STATUS_OK) {
                    return PHIPIA_CAMERA_STATUS_SURFACE_FAILURE;
                }
                thumb_pixels[y * PHIPIA_CAMERA_THUMB + x] = pixel;
            }
        }
        thumb_valid = true;
    }
    ui_motion_reset(&shutter, 0);
    ui_motion_to(&shutter, (int32_t)UI_MOTION_ONE, UI_MOTION_FLASH_NS,
        clock_monotonic_ns());
    shutter_running = true;
    *damage = rect_join(view, control_damage(PHIPIA_CAMERA_CONTROL_SHUTTER));
    return PHIPIA_CAMERA_STATUS_OK;
}

/* =============================================================== ANIMATION */

bool phipia_camera_animate(struct ui_rect *damage)
{
    const uint64_t now = clock_monotonic_ns();
    bool moved = false;

    if (damage == NULL || !initialized) {
        return false;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    for (int control = 0; control < PHIPIA_CAMERA_CONTROL_COUNT; ++control) {
        if (ui_motion_advance(&hover[control], now, ui_ease_linear)) {
            *damage = rect_join(*damage,
                control_damage((enum phipia_camera_control)control));
            moved = true;
        }
    }
    if (ui_motion_advance(&mode_shift, now, ui_ease_standard)) {
        *damage = rect_join(*damage, top_bar_rect());
        moved = true;
    }
    if (ui_motion_advance(&shutter, now, ui_ease_standard)) {
        *damage = rect_join(*damage, rect_join(phipia_camera_viewfinder_bounds(),
            control_damage(PHIPIA_CAMERA_CONTROL_SHUTTER)));
        moved = true;
        if (!ui_motion_running(&shutter)) {
            shutter_running = false;
        }
    }
    return moved;
}

bool phipia_camera_animating(void)
{
    if (ui_motion_running(&mode_shift) || ui_motion_running(&shutter)) {
        return true;
    }
    for (int control = 0; control < PHIPIA_CAMERA_CONTROL_COUNT; ++control) {
        if (ui_motion_running(&hover[control])) {
            return true;
        }
    }
    return false;
}

/* ============================================================== LIFECYCLE */

enum phipia_camera_status phipia_camera_set_frame(struct ui_rect frame)
{
    /* Wide enough for the timer, the segment group and the two bar buttons
     * without them touching, and tall enough for both bars and a frame. */
    const uint32_t least_width = PHIPIA_CAMERA_BORDER * 2U + PHIPIA_CAMERA_BAR_INSET +
        PHIPIA_CAMERA_GLYPH + PHIPIA_CAMERA_SEGMENT_WIDTH * 2U + PHIPIA_CAMERA_SEGMENT_GAP +
        PHIPIA_CAMERA_BAR_BUTTON * 2U + 32U;
    const uint32_t least_height = PHIPIA_CAMERA_BORDER * 2U + PHIPIA_CAMERA_TOP_BAR +
        PHIPIA_CAMERA_BOTTOM_BAR + 120U;

    if (frame.width < least_width || frame.height < least_height) {
        return PHIPIA_CAMERA_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return PHIPIA_CAMERA_STATUS_OK;
}

enum phipia_camera_status phipia_camera_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum phipia_camera_status status;

    if (target == NULL) {
        return PHIPIA_CAMERA_STATUS_NULL_ARGUMENT;
    }
    canvas = target;
    status = phipia_camera_set_frame(frame);
    if (status != PHIPIA_CAMERA_STATUS_OK) {
        return status;
    }
    for (int control = 0; control < PHIPIA_CAMERA_CONTROL_COUNT; ++control) {
        ui_motion_reset(&hover[control], 0);
    }
    ui_motion_reset(&mode_shift, current_mode == PHIPIA_CAMERA_MODE_VIDEO ?
        (int32_t)UI_MOTION_ONE : 0);
    ui_motion_reset(&shutter, 0);
    shutter_running = false;
    thumb_valid = false;
    initialized = true;
    return PHIPIA_CAMERA_STATUS_OK;
}

enum phipia_camera_status phipia_camera_set_mode(enum phipia_camera_mode mode)
{
    if (mode >= PHIPIA_CAMERA_MODE_COUNT) {
        return PHIPIA_CAMERA_STATUS_BAD_INDEX;
    }
    current_mode = mode;
    /* The segmented control's marker SLIDES, so it takes Faster under its
     * own name rather than the brush alias - it is a move on a standard
     * curve, not a colour cross-fade on a linear one. */
    ui_motion_to(&mode_shift, mode == PHIPIA_CAMERA_MODE_VIDEO ?
        (int32_t)UI_MOTION_ONE : 0, UI_MOTION_FASTER_NS,
        clock_monotonic_ns());
    return PHIPIA_CAMERA_STATUS_OK;
}

enum phipia_camera_status phipia_camera_set_feed(bool present)
{
    feed_present = present;
    return PHIPIA_CAMERA_STATUS_OK;
}

enum phipia_camera_status phipia_camera_set_focus(bool active)
{
    focused = active;
    return PHIPIA_CAMERA_STATUS_OK;
}

/* ============================================================== SELF TEST */

bool phipia_camera_self_test(void)
{
    static const char *const marks[] = { "timer", "camera", "video" };
    const struct ui_rect bar = top_bar_rect();
    const struct ui_rect view = phipia_camera_viewfinder_bounds();
    const struct ui_rect shutter_box = phipia_camera_capture_bounds();
    const struct ui_rect thumb = control_rect(PHIPIA_CAMERA_CONTROL_THUMB);
    const struct ui_rect timer = control_rect(PHIPIA_CAMERA_CONTROL_TIMER);
    const struct ui_rect photo = control_rect(PHIPIA_CAMERA_CONTROL_PHOTO);
    const struct ui_rect video = control_rect(PHIPIA_CAMERA_CONTROL_VIDEO);
    const struct ui_rect menu = control_rect(PHIPIA_CAMERA_CONTROL_MENU);

    if (phipia_camera_set_mode(PHIPIA_CAMERA_MODE_COUNT) != PHIPIA_CAMERA_STATUS_BAD_INDEX) {
        self_test_failure = "camera accepted a mode past the end";
        return false;
    }
    /*
     * Every cell has to exist AT the size drawn.  This window composites one
     * pixel to one pixel and never resamples, so a cell cut at some other
     * size is not a fallback - it is a mark that is not as crisp as the one
     * beside it, which is the fault that got two earlier versions of this
     * window thrown away.
     */
    for (size_t index = 0U; index < sizeof(marks) / sizeof(marks[0]);
         ++index) {
        if (glyph_cell(marks[index], PHIPIA_CAMERA_GLYPH) == NULL) {
            self_test_failure = "a camera mark was not cut at the size it "
                "is drawn";
            return false;
        }
    }
    /* And its box has to centre it on a whole pixel: an odd margin lands a
     * hinted stroke on a half pixel. */
    if ((PHIPIA_CAMERA_SEGMENT_WIDTH - PHIPIA_CAMERA_GLYPH) % 2U != 0U ||
            (PHIPIA_CAMERA_SEGMENT_HEIGHT - PHIPIA_CAMERA_GLYPH) % 2U != 0U ||
            (PHIPIA_CAMERA_TOP_BAR - PHIPIA_CAMERA_GLYPH) % 2U != 0U) {
        self_test_failure = "a camera mark does not centre on a pixel";
        return false;
    }
    if (PHIPIA_CAMERA_SEGMENT_HEIGHT < PHIPIA_CAMERA_GLYPH) {
        self_test_failure = "the mode segments are shorter than their marks";
        return false;
    }
    if (!initialized) {
        self_test_failure = "";
        return true;
    }
    if (view.width == 0U || view.height == 0U) {
        self_test_failure = "the viewfinder has no area";
        return false;
    }
    /* Four to three, to the pixel the integer division allows. */
    if (view.width * 3U / 4U != view.height &&
            view.height * 4U / 3U != view.width) {
        self_test_failure = "the viewfinder is not four to three";
        return false;
    }
    /* The shutter is centred on the WINDOW, which is what the arrangement
     * is recognised by; a shutter that drifts off centre with the window's
     * width is the one thing here that cannot be allowed to. */
    if (shutter_box.x + PHIPIA_CAMERA_SHUTTER / 2U !=
            inner_rect().x + inner_rect().width / 2U) {
        self_test_failure = "the shutter is not centred in the window";
        return false;
    }
    if (thumb.x + thumb.width > shutter_box.x) {
        self_test_failure = "the thumbnail runs into the shutter";
        return false;
    }
    if (thumb.x < inner_rect().x) {
        self_test_failure = "the thumbnail falls off the left of the bar";
        return false;
    }
    /* The top bar's three groups must not collide: the timer, the centred
     * segments, and the two buttons on the right. */
    if (timer.x + timer.width >= photo.x) {
        self_test_failure = "the timer runs into the mode segments";
        return false;
    }
    if (video.x + video.width >= menu.x) {
        self_test_failure = "the mode segments run into the menu";
        return false;
    }
    if (bar.width == 0U) {
        self_test_failure = "the top bar has no width";
        return false;
    }
    /*
     * The shared motion.  A curve that did not start at nought and end at
     * one would put the flash half a shade out, and one that is not half
     * done halfway through is a step function - which passes an endpoint
     * check and a monotonicity check without easing anything.
     */
    if (ui_ease_standard(0) != 0 ||
            ui_ease_standard((int32_t)UI_MOTION_ONE) !=
                (int32_t)UI_MOTION_ONE) {
        self_test_failure = "the standard easing does not span nought to one";
        return false;
    }
    if (ui_ease_standard((int32_t)(UI_MOTION_ONE / 2)) <
            (int32_t)(UI_MOTION_ONE * 40 / 100) ||
            ui_ease_standard((int32_t)(UI_MOTION_ONE / 2)) >
                (int32_t)(UI_MOTION_ONE * 60 / 100)) {
        self_test_failure = "the standard easing is not half done halfway";
        return false;
    }
    self_test_failure = "";
    return true;
}

const char *phipia_camera_self_test_failure(void)
{
    return self_test_failure;
}
