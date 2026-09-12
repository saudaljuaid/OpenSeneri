/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/ui_anim.h>

#include <phipia/heap.h>
#include <phipia/surface.h>

/*
 * Fixed point, 16.16 throughout. Products widen to 64 bits before they are
 * taken back down, because the interesting ones - a ratio times a ratio
 * times a coordinate - overflow 32 bits well inside the range the layout
 * actually uses.
 */
typedef int64_t ui_anim_fixed;

static const char *self_test_failure;

static ui_anim_fixed fixed_clamp(ui_anim_fixed value)
{
    if (value < 0) {
        return 0;
    }
    if (value > (ui_anim_fixed)UI_ANIM_ONE) {
        return (ui_anim_fixed)UI_ANIM_ONE;
    }
    return value;
}

static ui_anim_fixed fixed_multiply(ui_anim_fixed a, ui_anim_fixed b)
{
    return (a * b) / (ui_anim_fixed)UI_ANIM_ONE;
}

static ui_anim_fixed fixed_lerp(ui_anim_fixed a, ui_anim_fixed b,
    ui_anim_fixed t)
{
    return a + fixed_multiply(b - a, t);
}

/*
 * t^3 * (t * (6t - 15) + 10). The polynomial stays positive across the unit
 * interval - it is 10 at zero and 1 at one - so nothing here goes negative
 * on the way to the result.
 */
uint32_t ui_anim_ease(uint32_t t)
{
    ui_anim_fixed x = fixed_clamp((ui_anim_fixed)t);
    ui_anim_fixed x2 = fixed_multiply(x, x);
    ui_anim_fixed x3 = fixed_multiply(x2, x);
    ui_anim_fixed inner = (6 * x2) - (15 * x) + (10 * (ui_anim_fixed)UI_ANIM_ONE);

    return (uint32_t)fixed_clamp(fixed_multiply(x3, inner));
}

/* ui_anim_ease of a value remapped so that `lo` is zero and `hi` is one. */
static ui_anim_fixed ease_band(ui_anim_fixed lo, ui_anim_fixed hi,
    ui_anim_fixed value)
{
    ui_anim_fixed span = hi - lo;

    if (span <= 0) {
        return value < lo ? 0 : (ui_anim_fixed)UI_ANIM_ONE;
    }
    return (ui_anim_fixed)ui_anim_ease((uint32_t)fixed_clamp(
        ((value - lo) * (ui_anim_fixed)UI_ANIM_ONE) / span));
}

void ui_anim_reset(struct ui_anim *anim)
{
    if (anim == NULL) {
        return;
    }
    anim->running = false;
    anim->opening = false;
    anim->start_ns = 0U;
    anim->duration_ns = 0U;
    anim->origin = (struct ui_rect){ 0U, 0U, 0U, 0U };
    anim->frame = (struct ui_rect){ 0U, 0U, 0U, 0U };
    anim->pixels = NULL;
    anim->row = NULL;
    anim->progress = 0U;
}

bool ui_anim_running(const struct ui_anim *anim)
{
    return anim != NULL && anim->running && anim->pixels != NULL;
}

void ui_anim_end(struct ui_anim *anim)
{
    if (anim == NULL) {
        return;
    }
    if (anim->pixels != NULL) {
        (void)heap_free(anim->pixels);
    }
    ui_anim_reset(anim);
}

enum ui_anim_status ui_anim_begin(
    struct ui_anim *anim,
    const struct surface *source,
    struct ui_rect frame,
    struct ui_rect origin,
    bool opening,
    uint64_t now_ns,
    uint64_t duration_ns
)
{
    uint64_t count;
    uint64_t bytes;
    void *allocation = NULL;

    if (anim == NULL || source == NULL) {
        return UI_ANIM_STATUS_NULL_ARGUMENT;
    }
    if (source->pixels == NULL ||
        source->pitch % SURFACE_BYTES_PER_PIXEL != 0U ||
        source->pitch / SURFACE_BYTES_PER_PIXEL < source->width ||
        frame.width == 0U || frame.height == 0U ||
        origin.width == 0U || origin.height == 0U ||
        frame.width > UI_ANIM_MAX_WIDTH ||
        origin.width > frame.width || duration_ns == 0U) {
        return UI_ANIM_STATUS_BAD_GEOMETRY;
    }
    if (frame.x > source->width ||
        frame.width > source->width - frame.x ||
        frame.y > source->height ||
        frame.height > source->height - frame.y ||
        origin.x > UINT32_MAX - origin.width ||
        origin.y > UINT32_MAX - origin.height) {
        return UI_ANIM_STATUS_BAD_GEOMETRY;
    }

    ui_anim_end(anim);

    /* the picture, and one scratch row for the resampled span */
    count = (uint64_t)frame.width * (uint64_t)frame.height +
        (uint64_t)frame.width;
    bytes = count * (uint64_t)SURFACE_BYTES_PER_PIXEL;
    if (heap_allocate(bytes, &allocation) != HEAP_STATUS_OK ||
        allocation == NULL) {
        return UI_ANIM_STATUS_NO_MEMORY;
    }

    anim->pixels = (uint32_t *)allocation;
    anim->row = anim->pixels + (size_t)frame.width * (size_t)frame.height;

    /* pitch is bytes between rows, not pixels -- the surface says so and
     * a surface may be bound to a framebuffer whose rows are padded */
    const size_t stride = (size_t)source->pitch / SURFACE_BYTES_PER_PIXEL;

    for (uint32_t y = 0U; y < frame.height; ++y) {
        const uint32_t *line = source->pixels +
            (size_t)(frame.y + y) * stride + (size_t)frame.x;
        uint32_t *out = anim->pixels + (size_t)y * (size_t)frame.width;

        for (uint32_t x = 0U; x < frame.width; ++x) {
            out[x] = line[x];
        }
    }

    anim->frame = frame;
    anim->origin = origin;
    anim->opening = opening;
    anim->start_ns = now_ns;
    anim->duration_ns = duration_ns;
    anim->running = true;
    anim->progress = opening ? 0U : UI_ANIM_ONE;
    return UI_ANIM_STATUS_OK;
}

uint32_t ui_anim_advance(struct ui_anim *anim, uint64_t now_ns)
{
    uint64_t elapsed;
    uint32_t linear;

    if (!ui_anim_running(anim)) {
        return anim == NULL ? 0U : anim->progress;
    }
    elapsed = now_ns > anim->start_ns ? now_ns - anim->start_ns : 0U;
    if (elapsed >= anim->duration_ns) {
        anim->progress = anim->opening ? UI_ANIM_ONE : 0U;
        anim->running = false;
        return anim->progress;
    }
    linear = (uint32_t)((elapsed * (uint64_t)UI_ANIM_ONE) / anim->duration_ns);
    anim->progress = anim->opening ? linear : UI_ANIM_ONE - linear;
    return anim->progress;
}

struct ui_rect ui_anim_bounds(const struct ui_anim *anim)
{
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;

    if (anim == NULL) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    left = anim->frame.x < anim->origin.x ? anim->frame.x : anim->origin.x;
    top = anim->frame.y < anim->origin.y ? anim->frame.y : anim->origin.y;
    right = anim->frame.x + anim->frame.width;
    if (anim->origin.x + anim->origin.width > right) {
        right = anim->origin.x + anim->origin.width;
    }
    bottom = anim->frame.y + anim->frame.height;
    if (anim->origin.y + anim->origin.height > bottom) {
        bottom = anim->origin.y + anim->origin.height;
    }
    return (struct ui_rect){ left, top, right - left, bottom - top };
}

/*
 * The shape of one frame.
 *
 * `top` and `bottom` are where the warped picture starts and stops, and they
 * are eased apart: the bottom edge runs to the dock early and the top edge
 * follows late, so the picture is stretched between them rather than scaled
 * down towards them. `pinch` releases on a third curve, a little ahead of
 * the edges, so the neck has opened before the shape finishes travelling.
 */
struct ui_anim_shape {
    ui_anim_fixed top;
    ui_anim_fixed bottom;
    ui_anim_fixed pinch;      /* 1 fully drawn into the icon, 0 fully open */
    ui_anim_fixed narrow;     /* icon width over frame width */
};

static struct ui_anim_shape shape_for(const struct ui_anim *anim,
    uint32_t progress)
{
    const ui_anim_fixed p = fixed_clamp((ui_anim_fixed)progress);
    const ui_anim_fixed one = (ui_anim_fixed)UI_ANIM_ONE;
    struct ui_anim_shape shape;
    ui_anim_fixed top_ease;
    ui_anim_fixed bottom_ease;

    top_ease = (ui_anim_fixed)ui_anim_ease(
        (uint32_t)fixed_clamp((p * 155) / 100));
    bottom_ease = ease_band((22 * one) / 100, one, p);
    shape.pinch = one - ease_band((10 * one) / 100, (85 * one) / 100, p);

    shape.top = fixed_lerp((ui_anim_fixed)anim->origin.y * one,
        (ui_anim_fixed)anim->frame.y * one, top_ease);
    shape.bottom = fixed_lerp(
        (ui_anim_fixed)(anim->origin.y + anim->origin.height) * one,
        (ui_anim_fixed)(anim->frame.y + anim->frame.height) * one,
        bottom_ease);
    shape.narrow = ((ui_anim_fixed)anim->origin.width * one) /
        (ui_anim_fixed)anim->frame.width;
    return shape;
}

enum ui_anim_status ui_anim_draw(
    struct ui_anim *anim,
    struct surface *target,
    struct ui_rect clip
)
{
    struct ui_anim_shape shape;
    const ui_anim_fixed one = (ui_anim_fixed)UI_ANIM_ONE;
    ui_anim_fixed span;
    ui_anim_fixed frame_centre;
    ui_anim_fixed origin_centre;
    ui_anim_fixed neck;
    int64_t first_row;
    int64_t last_row;
    uint32_t clip_left;
    uint32_t clip_right;

    if (anim == NULL || target == NULL) {
        return UI_ANIM_STATUS_NULL_ARGUMENT;
    }
    if (anim->pixels == NULL || anim->row == NULL ||
        anim->frame.width == 0U || anim->frame.height == 0U ||
        anim->frame.width > UI_ANIM_MAX_WIDTH ||
        target->pixels == NULL ||
        target->pitch % SURFACE_BYTES_PER_PIXEL != 0U ||
        target->pitch / SURFACE_BYTES_PER_PIXEL < target->width) {
        return UI_ANIM_STATUS_NOT_RUNNING;
    }
    if (clip.width == 0U || clip.height == 0U) {
        return UI_ANIM_STATUS_OK;
    }
    if (clip.x > target->width || clip.width > target->width - clip.x ||
        clip.y > target->height || clip.height > target->height - clip.y) {
        return UI_ANIM_STATUS_BAD_GEOMETRY;
    }

    shape = shape_for(anim, anim->progress);
    span = shape.bottom - shape.top;
    if (span < one) {
        return UI_ANIM_STATUS_OK;
    }
    neck = one - shape.pinch;
    frame_centre = ((ui_anim_fixed)anim->frame.x * 2 +
        (ui_anim_fixed)anim->frame.width) * one / 2;
    origin_centre = ((ui_anim_fixed)anim->origin.x * 2 +
        (ui_anim_fixed)anim->origin.width) * one / 2;

    first_row = shape.top / one;
    last_row = (shape.bottom + one - 1) / one;
    if (first_row < (int64_t)clip.y) {
        first_row = (int64_t)clip.y;
    }
    if (last_row > (int64_t)clip.y + (int64_t)clip.height) {
        last_row = (int64_t)clip.y + (int64_t)clip.height;
    }
    clip_left = clip.x;
    clip_right = clip.x + clip.width;

    for (int64_t y = first_row; y < last_row; ++y) {
        /* how far down the warped shape this row sits */
        const ui_anim_fixed here = ((ui_anim_fixed)y * one) + (one / 2);
        const ui_anim_fixed down = fixed_clamp(
            ((here - shape.top) * one) / span);
        ui_anim_fixed pinched;
        ui_anim_fixed width_scale;
        ui_anim_fixed centre;
        ui_anim_fixed source_v;
        ui_anim_fixed inverse;
        int64_t left;
        int64_t width;
        int64_t start;
        int64_t stop;
        uint32_t source_row;
        uint32_t taps;
        uint32_t step;

        /*
         * The pinch front sits at `neck` and climbs as the shape is drawn
         * in. The band is wider below the front than above it, so the neck
         * tapers into the icon instead of stepping into it.
         */
        pinched = fixed_multiply(
            ease_band(neck - (one / 2), neck + (28 * one) / 100, down),
            shape.pinch);
        width_scale = fixed_lerp(one, shape.narrow, pinched);
        centre = fixed_lerp(frame_centre, origin_centre, pinched);

        width = (int64_t)fixed_multiply(
            (ui_anim_fixed)anim->frame.width * one, width_scale) / one;
        if (width < 1) {
            continue;
        }
        left = (centre / one) - (width / 2);

        /*
         * Ease-out sampling keeps the lower edge compressed at the neck and
         * blends toward linear sampling as the pinch releases.
         */
        inverse = one - down;
        source_v = fixed_lerp(down, one - fixed_multiply(inverse, inverse),
            shape.pinch);
        source_row = (uint32_t)(fixed_multiply(source_v,
            (ui_anim_fixed)(anim->frame.height - 1U) * one) / one);
        if (source_row >= anim->frame.height) {
            source_row = anim->frame.height - 1U;
        }

        start = left < (int64_t)clip_left ? (int64_t)clip_left : left;
        stop = left + width;
        if (stop > (int64_t)clip_right) {
            stop = (int64_t)clip_right;
        }
        if (start >= stop) {
            continue;
        }

        /*
         * Horizontally the picture is being minified, often severely, so a
         * single sample per destination pixel would alias the window's text
         * into noise. Averaging a bounded handful of taps across the source
         * span each destination pixel covers costs little and keeps the
         * neck legible.
         */
        taps = (uint32_t)((int64_t)anim->frame.width / width);
        if (taps < 1U) {
            taps = 1U;
        }
        if (taps > 4U) {
            taps = 4U;
        }
        step = (uint32_t)((int64_t)anim->frame.width * (int64_t)one /
            (width * (int64_t)taps));

        {
            const uint32_t *line = anim->pixels +
                (size_t)source_row * (size_t)anim->frame.width;

            for (int64_t x = start; x < stop; ++x) {
                const ui_anim_fixed across =
                    (((ui_anim_fixed)(x - left)) * one) / width;
                uint32_t base = (uint32_t)(fixed_multiply(across,
                    (ui_anim_fixed)anim->frame.width * one) / one);
                uint32_t red = 0U;
                uint32_t green = 0U;
                uint32_t blue = 0U;

                for (uint32_t tap = 0U; tap < taps; ++tap) {
                    uint32_t column = base + ((tap * step) / UI_ANIM_ONE);
                    uint32_t pixel;

                    if (column >= anim->frame.width) {
                        column = anim->frame.width - 1U;
                    }
                    pixel = line[column];
                    red += (pixel >> 16) & 0xFFU;
                    green += (pixel >> 8) & 0xFFU;
                    blue += pixel & 0xFFU;
                }
                anim->row[(size_t)(x - start)] =
                    ((red / taps) << 16) | ((green / taps) << 8) |
                    (blue / taps);
            }
        }

        if (surface_blit(target, (uint32_t)start, (uint32_t)y, anim->row,
                (uint32_t)(stop - start), 1U,
                (uint32_t)(stop - start) * SURFACE_BYTES_PER_PIXEL) !=
            SURFACE_STATUS_OK) {
            return UI_ANIM_STATUS_SURFACE_FAILURE;
        }
    }
    return UI_ANIM_STATUS_OK;
}

const char *ui_anim_status_string(enum ui_anim_status status)
{
    switch (status) {
    case UI_ANIM_STATUS_OK:
        return "ok";
    case UI_ANIM_STATUS_NULL_ARGUMENT:
        return "null argument";
    case UI_ANIM_STATUS_BAD_GEOMETRY:
        return "bad geometry";
    case UI_ANIM_STATUS_NO_MEMORY:
        return "no memory";
    case UI_ANIM_STATUS_NOT_RUNNING:
        return "not running";
    case UI_ANIM_STATUS_SURFACE_FAILURE:
        return "surface failure";
    default:
        return "unknown";
    }
}

const char *ui_anim_self_test_failure(void)
{
    return self_test_failure;
}

bool ui_anim_self_test(void)
{
    struct ui_anim anim;
    struct ui_rect frame = { 100U, 40U, 400U, 300U };
    struct ui_rect origin = { 300U, 600U, 80U, 80U };
    struct ui_rect bounds;
    uint32_t previous;

    self_test_failure = NULL;

    /* the easing has to hit both ends exactly, or nothing ever arrives */
    if (ui_anim_ease(0U) != 0U || ui_anim_ease(UI_ANIM_ONE) != UI_ANIM_ONE) {
        self_test_failure = "the easing curve does not reach its ends";
        return false;
    }
    if (ui_anim_ease(UI_ANIM_ONE / 2U) != UI_ANIM_ONE / 2U) {
        self_test_failure = "the easing curve is not symmetric at its middle";
        return false;
    }
    previous = 0U;
    for (uint32_t step = 1U; step <= 64U; ++step) {
        const uint32_t value = ui_anim_ease(step * (UI_ANIM_ONE / 64U));

        if (value < previous) {
            self_test_failure = "the easing curve is not monotonic";
            return false;
        }
        previous = value;
    }

    ui_anim_reset(&anim);
    if (ui_anim_running(&anim)) {
        self_test_failure = "a reset animation reported itself running";
        return false;
    }
    if (ui_anim_advance(&anim, 1000U) != 0U) {
        self_test_failure = "a stopped animation advanced";
        return false;
    }

    /* geometry is checked without a surface, so the failure paths are real */
    if (ui_anim_begin(&anim, NULL, frame, origin, true, 0U,
            UI_ANIM_DEFAULT_OPEN_NS) != UI_ANIM_STATUS_NULL_ARGUMENT) {
        self_test_failure = "a null source was accepted";
        return false;
    }

    anim.frame = frame;
    anim.origin = origin;
    bounds = ui_anim_bounds(&anim);
    if (bounds.x != 100U || bounds.y != 40U ||
        bounds.x + bounds.width != 500U ||
        bounds.y + bounds.height != 680U) {
        self_test_failure = "the animation bounds do not cover both ends";
        return false;
    }

    /*
     * The shape has to start folded into the icon and finish exactly on the
     * panel, because those two frames are the ones the rest of the drawing
     * assumes it can take over from.
     */
    {
        struct ui_anim_shape closed = shape_for(&anim, 0U);
        struct ui_anim_shape open = shape_for(&anim, UI_ANIM_ONE);

        if (closed.top / (ui_anim_fixed)UI_ANIM_ONE !=
                (ui_anim_fixed)origin.y ||
            closed.pinch != (ui_anim_fixed)UI_ANIM_ONE) {
            self_test_failure = "the closed shape is not the dock icon";
            return false;
        }
        if (open.top / (ui_anim_fixed)UI_ANIM_ONE != (ui_anim_fixed)frame.y ||
            open.bottom / (ui_anim_fixed)UI_ANIM_ONE !=
                (ui_anim_fixed)(frame.y + frame.height) ||
            open.pinch != 0) {
            self_test_failure = "the open shape is not the panel";
            return false;
        }
    }

    ui_anim_reset(&anim);
    return true;
}
