/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * The shell's motion, in one place.
 *
 * Every window that fades a hover, slides a panel or flashes a shutter wants
 * the same three things: a value that travels from one number to another over
 * a duration, an easing curve to travel it on, and a way to ask whether it has
 * arrived. Six copies of that is six places for it to drift, so it is a header
 * of static inline functions rather than a module - a kernel with no
 * link-time optimisation should not pay a call for an interpolation.
 *
 * EVERY DURATION HERE IS ONE OF THE FOUR Windows 10 XAML control durations,
 * and every curve is a published Fluent bezier. There are no numbers in this
 * file that were chosen because they looked right - an earlier version had
 * three, at 150, 250 and 320 milliseconds, and two of them were a near miss
 * for a real resource that was sitting there the whole time.
 *
 * Everything is Q16.16 fixed point. The kernel builds with -msoft-float and no
 * SSE, so a float here would be a call into a soft-float library on every
 * pixel of every frame.
 */
#ifndef PHIPIA_UI_MOTION_H
#define PHIPIA_UI_MOTION_H

#include <stdbool.h>
#include <stdint.h>

/* One, in Q16.16 - the value every motion travels to. */
#define UI_MOTION_ONE INT64_C(65536)
/* A bezier control point written as a thousandth, so 800 is 0.8. */
#define UI_MOTION_BEZIER(v) ((int32_t)((v) * UI_MOTION_ONE / 1000))

/*
 * TIMINGS, all four of them Windows'.
 *
 * Windows 10's XAML theme resources define exactly four control durations,
 * and between them they cover everything this shell animates:
 *
 *   ControlFasterAnimationDuration  0:0:0.083
 *   ControlFastAnimationDuration    0:0:0.167
 *   ControlNormalAnimationDuration  0:0:0.250
 *   ControlSlowAnimationDuration    0:0:0.333
 *
 * The 83 is also what every fill and border colour change on a taskbar
 * button is declared as inline on BackgroundElement - a BrushTransition,
 * which has no pronounced easing - so a hover cross-fade is 83 milliseconds
 * and linear, everywhere.
 *
 * The other three used to be 250, 150 and 320.  The 250 was already
 * ControlNormalAnimationDuration and nobody had noticed; the other two were
 * within a frame and a half of a real resource and were rounded to it.
 * Nothing in this shell is timed to a number somebody liked the look of.
 */
#define UI_MOTION_FASTER_NS UINT64_C(83000000)     /* Faster: 0:0:0.083 */
#define UI_MOTION_FAST_NS UINT64_C(167000000)      /* Fast:   0:0:0.167 */
#define UI_MOTION_REVEAL_NS UINT64_C(250000000)    /* Normal: 0:0:0.250 */
#define UI_MOTION_SLOW_NS UINT64_C(333000000)      /* Slow:   0:0:0.333 */
/*
 * The same four durations under the names of what they are FOR, so a call
 * site says why it picked one.  A brush cross-fade and a small control
 * sliding are both Faster and are not the same thing: the first is a
 * BrushTransition and genuinely does not ease, the second is a move that
 * does, and one alias each keeps a linear curve from being pasted onto a
 * slide because the constant had "brush" in its name.
 */
#define UI_MOTION_BRUSH_NS UI_MOTION_FASTER_NS
#define UI_MOTION_DISMISS_NS UI_MOTION_FAST_NS
#define UI_MOTION_FLASH_NS UI_MOTION_SLOW_NS

struct ui_motion {
    int32_t value;    /* Q16.16, 0 .. UI_MOTION_ONE */
    int32_t from;
    int32_t target;
    uint64_t start_ns;
    uint64_t duration_ns;
    bool running;
};

/*
 * A cubic bezier evaluated by bisecting x for t and reading y.
 *
 * Newton would be faster and is not worth it: twenty bisections of a
 * sixteen-bit fraction is exact to the last bit, runs a fixed number of
 * iterations whatever the curve, and cannot diverge on the flat start of an
 * ease-in - which is exactly where Newton is worst.
 */
static inline int32_t ui_bezier(int32_t x1, int32_t y1, int32_t x2,
    int32_t y2, int32_t progress)
{
    int64_t low = 0;
    int64_t high = UI_MOTION_ONE;

    if (progress <= 0) {
        return 0;
    }
    if (progress >= (int32_t)UI_MOTION_ONE) {
        return (int32_t)UI_MOTION_ONE;
    }
    for (int step = 0; step < 20; ++step) {
        const int64_t t = (low + high) / 2;
        const int64_t u = UI_MOTION_ONE - t;
        /* 3(1-t)^2 t x1 + 3(1-t) t^2 x2 + t^3, in Q16.16 throughout. */
        const int64_t x = (3 * u * u / UI_MOTION_ONE * t / UI_MOTION_ONE *
            x1 / UI_MOTION_ONE) + (3 * u * t / UI_MOTION_ONE * t /
            UI_MOTION_ONE * x2 / UI_MOTION_ONE) + (t * t / UI_MOTION_ONE *
            t / UI_MOTION_ONE);

        if (x < progress) {
            low = t;
        } else {
            high = t;
        }
    }

    const int64_t t = (low + high) / 2;
    const int64_t u = UI_MOTION_ONE - t;
    const int64_t y = (3 * u * u / UI_MOTION_ONE * t / UI_MOTION_ONE * y1 /
        UI_MOTION_ONE) + (3 * u * t / UI_MOTION_ONE * t / UI_MOTION_ONE *
        y2 / UI_MOTION_ONE) + (t * t / UI_MOTION_ONE * t / UI_MOTION_ONE);

    return (int32_t)y;
}

/* A BrushTransition does not ease. */
static inline int32_t ui_ease_linear(int32_t progress)
{
    return progress;
}

/* FluentStandard: enters and leaves at rest. */
static inline int32_t ui_ease_standard(int32_t progress)
{
    return ui_bezier(UI_MOTION_BEZIER(800), UI_MOTION_BEZIER(0),
        UI_MOTION_BEZIER(200), UI_MOTION_BEZIER(1000), progress);
}

/*
 * ControlFastOutSlowInKeySpline - "0.1,0.9,0.2,1.0" in the theme resources,
 * and the curve Windows puts on nearly everything that ARRIVES, from a
 * checkbox tick to a popup.  Fast in, settles.
 */
static inline int32_t ui_ease_decelerate(int32_t progress)
{
    return ui_bezier(UI_MOTION_BEZIER(100), UI_MOTION_BEZIER(900),
        UI_MOTION_BEZIER(200), UI_MOTION_BEZIER(1000), progress);
}

/* FluentAccelerate: slow out, fast away.  Used by things that leave. */
static inline int32_t ui_ease_accelerate(int32_t progress)
{
    return ui_bezier(UI_MOTION_BEZIER(700), UI_MOTION_BEZIER(0),
        UI_MOTION_BEZIER(1000), UI_MOTION_BEZIER(500), progress);
}

static inline void ui_motion_reset(struct ui_motion *motion, int32_t value)
{
    motion->value = value;
    motion->from = value;
    motion->target = value;
    motion->start_ns = 0U;
    motion->duration_ns = 0U;
    motion->running = false;
}

static inline void ui_motion_to(struct ui_motion *motion, int32_t target,
    uint64_t duration_ns, uint64_t now_ns)
{
    if (motion->target == target &&
            (motion->running || motion->value == target)) {
        return;
    }
    motion->from = motion->value;
    motion->target = target;
    motion->start_ns = now_ns;
    motion->duration_ns = duration_ns == 0U ? 1U : duration_ns;
    motion->running = motion->value != target;
    if (!motion->running) {
        motion->value = target;
    }
}

/* Advance to the clock.  Returns true when the value moved. */
static inline bool ui_motion_advance(struct ui_motion *motion,
    uint64_t now_ns, int32_t (*curve)(int32_t))
{
    if (!motion->running) {
        return false;
    }

    const uint64_t elapsed = now_ns >= motion->start_ns ?
        now_ns - motion->start_ns : 0U;

    if (elapsed >= motion->duration_ns) {
        motion->value = motion->target;
        motion->running = false;
        return true;
    }

    const int32_t linear = (int32_t)((int64_t)elapsed * UI_MOTION_ONE /
        (int64_t)motion->duration_ns);
    const int32_t eased = curve(linear);

    motion->value = motion->from + (int32_t)(((int64_t)motion->target -
        motion->from) * eased / UI_MOTION_ONE);
    return true;
}

/* The value as a 0..255 alpha, which is what a fade actually wants. */
static inline uint32_t ui_motion_alpha(const struct ui_motion *motion)
{
    const int32_t value = motion->value < 0 ? 0 :
        (motion->value > (int32_t)UI_MOTION_ONE ?
            (int32_t)UI_MOTION_ONE : motion->value);

    return (uint32_t)((int64_t)value * 255 / UI_MOTION_ONE);
}

static inline bool ui_motion_running(const struct ui_motion *motion)
{
    return motion->running;
}

#endif
