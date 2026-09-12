/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  Phipia timer backend addition for SDL 2.32.10. This file is distributed
  under the same zlib license as SDL.
*/

#include "../../SDL_internal.h"

#ifdef SDL_TIMER_PHIPIA

#include "SDL_timer.h"

#include <phipia/runtime.h>

static Uint64 phipia_ticks_origin;
static SDL_bool phipia_ticks_started = SDL_FALSE;

void SDL_TicksInit(void)
{
    if (!phipia_ticks_started) {
        phipia_ticks_origin = phipia_monotonic_ns();
        phipia_ticks_started = SDL_TRUE;
    }
}

void SDL_TicksQuit(void)
{
    phipia_ticks_started = SDL_FALSE;
    phipia_ticks_origin = 0U;
}

Uint64 SDL_GetTicks64(void)
{
    Uint64 now;

    if (!phipia_ticks_started) {
        SDL_TicksInit();
    }
    now = phipia_monotonic_ns();
    return now >= phipia_ticks_origin ?
        (now - phipia_ticks_origin) / UINT64_C(1000000) : 0U;
}

Uint64 SDL_GetPerformanceCounter(void)
{
    return phipia_monotonic_ns();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return UINT64_C(1000000000);
}

void SDL_Delay(Uint32 milliseconds)
{
    const Uint64 now = phipia_monotonic_ns();
    const Uint64 delta = (Uint64)milliseconds * UINT64_C(1000000);
    const Uint64 deadline = delta > UINT64_MAX - now ? UINT64_MAX :
        now + delta;

    (void)phipia_sleep_until(deadline);
}

#endif /* SDL_TIMER_PHIPIA */
