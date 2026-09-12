/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * The pointer.  See include/phipia/cursor.h for what this is, what it is
 * not, and the two Windows cursors it does not draw.
 */

#include <phipia/cursor.h>

#include <phipia/clock.h>
#include <phipia/framebuffer.h>

#include "cursor_art.h"

/*
 * The busy ring's rotation.
 *
 * The one animation in this shell that the four XAML control durations do
 * not cover, and correctly so: a cursor is not a control.  An animated
 * Windows cursor is a .ani file whose rate chunk counts JIFFIES - sixtieths
 * of a second - so whatever Windows chose for aniwait.ani, it is a whole
 * number of them, and a rate that is not is a rate Windows could not have
 * specified.
 *
 * Six jiffies is 100 ms exactly, eight frames of it is 800 ms for a full
 * turn, and that reads as a spinner rather than a flicker.  The QUANTUM is
 * sourced and the count of them is not; this project has no copy of the
 * file to measure.  The self-test holds it to a whole number of jiffies,
 * which is the part that can be checked.
 */
#define CURSOR_JIFFY_NS UINT64_C(16666667)         /* 1/60 s, the .ani unit */
#define CURSOR_RING_FRAME_NS (CURSOR_JIFFY_NS * 6U)

/*
 * Where the pointer actually IS inside each cursor's canvas - the one pixel
 * a click lands on.  Read off the shapes tools/make-cursors.py draws rather
 * than a Windows .cur file, which this project has no copy of; marked
 * PENDING VERIFICATION for the same reason the shapes themselves are.
 */
static const struct ui_point cursor_hotspot[CURSOR_KIND_COUNT] = {
    [CURSOR_NORMAL_SELECT] = { 2, 2 },
    [CURSOR_HELP_SELECT] = { 2, 2 },
    [CURSOR_WORKING_IN_BACKGROUND] = { 2, 2 },
    [CURSOR_BUSY] = { 16, 16 },
    [CURSOR_PRECISION_SELECT] = { 16, 16 },
    [CURSOR_TEXT_SELECT] = { 16, 16 },
    [CURSOR_HANDWRITING] = { 7, 29 },
    [CURSOR_UNAVAILABLE] = { 16, 16 },
    [CURSOR_VERTICAL_RESIZE] = { 16, 16 },
    [CURSOR_HORIZONTAL_RESIZE] = { 16, 16 },
    [CURSOR_DIAGONAL_RESIZE_1] = { 16, 16 },
    [CURSOR_DIAGONAL_RESIZE_2] = { 16, 16 },
    [CURSOR_MOVE] = { 16, 16 },
    [CURSOR_ALTERNATE_SELECT] = { 16, 4 },
    [CURSOR_LINK_SELECT] = { 11, 5 },
};

static struct surface *canvas;
static bool initialized;
static enum cursor_kind current_kind = CURSOR_NORMAL_SELECT;
static uint32_t ring_frame;
static uint64_t ring_started_ns;
static const char *self_test_failure = "cursor self-test has not run";

const char *cursor_status_string(enum cursor_status status)
{
    switch (status) {
    case CURSOR_STATUS_OK:
        return "ok";
    case CURSOR_STATUS_NULL_ARGUMENT:
        return "null argument";
    case CURSOR_STATUS_NOT_INITIALIZED:
        return "cursor not initialized";
    case CURSOR_STATUS_SURFACE_FAILURE:
        return "cursor surface refused a pixel";
    default:
        return "unknown cursor status";
    }
}

enum cursor_status cursor_initialize(struct surface *target)
{
    if (target == NULL) {
        return CURSOR_STATUS_NULL_ARGUMENT;
    }
    canvas = target;
    current_kind = CURSOR_NORMAL_SELECT;
    ring_frame = 0U;
    ring_started_ns = 0U;
    initialized = true;
    return CURSOR_STATUS_OK;
}

struct ui_rect cursor_placement(enum cursor_kind kind, struct ui_point at)
{
    const struct ui_point hotspot = cursor_hotspot[kind];
    const int32_t left = at.x - hotspot.x;
    const int32_t top = at.y - hotspot.y;

    return (struct ui_rect){
        left > 0 ? (uint32_t)left : 0U,
        top > 0 ? (uint32_t)top : 0U,
        CURSOR_ART_SIZE, CURSOR_ART_SIZE };
}

/* Whether `kind` is one of the two the ring animates - the only two whose
 * art depends on the clock rather than being fixed the moment it is
 * chosen. */
static bool cursor_is_ringed(enum cursor_kind kind)
{
    return kind == CURSOR_BUSY || kind == CURSOR_WORKING_IN_BACKGROUND;
}

enum cursor_status cursor_set_kind(enum cursor_kind kind)
{
    if ((size_t)kind >= CURSOR_KIND_COUNT) {
        return CURSOR_STATUS_NOT_INITIALIZED;
    }
    if (kind != current_kind) {
        /* A ring that was mid-turn when the pointer LEFT a busy state and
         * comes back to one later starts over rather than resuming - two
         * separate waits should not look like one continuous spin. */
        if (cursor_is_ringed(kind) && !cursor_is_ringed(current_kind)) {
            ring_frame = 0U;
            ring_started_ns = clock_monotonic_ns();
        }
        current_kind = kind;
    }
    return CURSOR_STATUS_OK;
}

enum cursor_kind cursor_get_kind(void)
{
    return current_kind;
}

bool cursor_animate(struct ui_rect *damage)
{
    uint64_t elapsed;
    uint32_t frame;

    if (damage == NULL) {
        return false;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    if (!initialized || !cursor_is_ringed(current_kind)) {
        return false;
    }
    elapsed = clock_monotonic_ns() - ring_started_ns;
    frame = (uint32_t)((elapsed / CURSOR_RING_FRAME_NS) %
        CURSOR_RING_FRAMES);
    if (frame == ring_frame) {
        return false;
    }
    ring_frame = frame;
    return true;
}

static enum cursor_status blit(const struct cursor_art_entry *art,
    struct ui_rect placed, struct ui_rect damage)
{
    const struct ui_rect clip = (struct ui_rect){
        placed.x > damage.x ? placed.x : damage.x,
        placed.y > damage.y ? placed.y : damage.y,
        0U, 0U };
    const uint32_t right = (placed.x + placed.width < damage.x + damage.width
        ? placed.x + placed.width : damage.x + damage.width);
    const uint32_t bottom = (placed.y + placed.height <
        damage.y + damage.height ? placed.y + placed.height :
        damage.y + damage.height);

    if (right <= clip.x || bottom <= clip.y) {
        return CURSOR_STATUS_OK;
    }
    for (uint32_t y = clip.y; y < bottom; ++y) {
        const uint32_t local_y = y - placed.y;

        for (uint32_t x = clip.x; x < right; ++x) {
            const uint32_t local_x = x - placed.x;
            const size_t offset = (size_t)local_y * CURSOR_ART_SIZE +
                local_x;
            const uint8_t coverage = art->alpha[offset];

            if (coverage == 0U) {
                continue;
            }
            if (coverage == 255U) {
                if (surface_pixel(canvas, x, y, art->pixels[offset]) !=
                        SURFACE_STATUS_OK) {
                    return CURSOR_STATUS_SURFACE_FAILURE;
                }
                continue;
            }
            {
                const struct framebuffer_state format =
                    framebuffer_get_state();
                const uint32_t over = art->pixels[offset];
                uint32_t under;
                uint32_t red;
                uint32_t green;
                uint32_t blue;

                if (surface_read_pixel(canvas, x, y, &under) !=
                        SURFACE_STATUS_OK) {
                    return CURSOR_STATUS_SURFACE_FAILURE;
                }
                red = (((over >> 16) & 0xFFU) * coverage +
                    ((under >> format.red_position) & 0xFFU) *
                    (255U - coverage) + 127U) / 255U;
                green = (((over >> 8) & 0xFFU) * coverage +
                    ((under >> format.green_position) & 0xFFU) *
                    (255U - coverage) + 127U) / 255U;
                blue = ((over & 0xFFU) * coverage +
                    ((under >> format.blue_position) & 0xFFU) *
                    (255U - coverage) + 127U) / 255U;
                if (surface_pixel(canvas, x, y,
                        (red << format.red_position) |
                        (green << format.green_position) |
                        (blue << format.blue_position)) !=
                            SURFACE_STATUS_OK) {
                    return CURSOR_STATUS_SURFACE_FAILURE;
                }
            }
        }
    }
    return CURSOR_STATUS_OK;
}

enum cursor_status cursor_draw(struct ui_point at, struct ui_rect damage)
{
    const struct ui_rect placed = cursor_placement(current_kind, at);
    const struct cursor_art_entry *art;

    if (!initialized) {
        return CURSOR_STATUS_NOT_INITIALIZED;
    }
    if (current_kind == CURSOR_BUSY) {
        art = &cursor_busy_art[ring_frame];
    } else if (current_kind == CURSOR_WORKING_IN_BACKGROUND) {
        art = &cursor_working_art[ring_frame];
    } else {
        art = &cursor_art[current_kind];
    }
    return blit(art, placed, damage);
}

/* ============================================================== SELF TEST */

bool cursor_self_test(void)
{
    /*
     * The ring's frame rate has to be a whole number of jiffies, because a
     * .ani file cannot express anything else - a rate that is not one is a
     * rate Windows could not have specified for the cursor this copies.
     */
    if (CURSOR_RING_FRAME_NS % CURSOR_JIFFY_NS != 0U ||
            CURSOR_RING_FRAME_NS == 0U) {
        self_test_failure = "the busy ring's rate is not a whole jiffy";
        return false;
    }
    for (size_t kind = 0U; kind < CURSOR_KIND_COUNT; ++kind) {
        const struct ui_point hotspot = cursor_hotspot[kind];

        if (hotspot.x < 0 || hotspot.y < 0 ||
                (uint32_t)hotspot.x >= CURSOR_ART_SIZE ||
                (uint32_t)hotspot.y >= CURSOR_ART_SIZE) {
            self_test_failure = "a cursor's hotspot falls outside its own "
                "canvas";
            return false;
        }
        if (kind == CURSOR_BUSY || kind == CURSOR_WORKING_IN_BACKGROUND) {
            continue;
        }
        if (cursor_art[kind].pixels == NULL ||
                cursor_art[kind].alpha == NULL) {
            self_test_failure = "a cursor kind has no art";
            return false;
        }
    }
    for (size_t frame = 0U; frame < CURSOR_RING_FRAMES; ++frame) {
        if (cursor_busy_art[frame].alpha == NULL ||
                cursor_working_art[frame].alpha == NULL) {
            self_test_failure = "a ring animation frame is missing";
            return false;
        }
    }
    /* Two frames of the ring have to actually DIFFER, or "the stages" are
     * one bitmap shown eight times. */
    {
        bool any_different = false;

        for (size_t offset = 0U;
             offset < (size_t)CURSOR_ART_SIZE * CURSOR_ART_SIZE; ++offset) {
            if (cursor_busy_art[0].alpha[offset] !=
                    cursor_busy_art[1].alpha[offset]) {
                any_different = true;
                break;
            }
        }
        if (!any_different) {
            self_test_failure = "the busy ring's frames are identical";
            return false;
        }
    }
    self_test_failure = "";
    return true;
}

const char *cursor_self_test_failure(void)
{
    return self_test_failure;
}
