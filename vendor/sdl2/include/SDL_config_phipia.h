/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  Phipia platform configuration. This file is an addition to the pinned
  SDL 2.32.10 source and remains under SDL's zlib license.
*/

#ifndef SDL_config_phipia_h_
#define SDL_config_phipia_h_

#include "SDL_platform.h"

#define SIZEOF_VOIDP 8
#define HAVE_GCC_ATOMICS 1

#define STDC_HEADERS 1
#define HAVE_CTYPE_H 1
#define HAVE_FLOAT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1

#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_GETENV 1
#define HAVE_QSORT 1
#define HAVE_BSEARCH 1
#define HAVE_ABS 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_STRTOD 1
#define HAVE_ATOI 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_VSNPRINTF 1
#define HAVE_SETJMP 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_NANOSLEEP 1

#define SDL_HAPTIC_DISABLED 1
#define SDL_HIDAPI_DISABLED 1
#define SDL_JOYSTICK_DISABLED 1
#define SDL_LOADSO_DISABLED 1
#define SDL_SENSOR_DISABLED 1

#define SDL_AUDIO_DRIVER_PHIPIA 1
#define SDL_THREAD_PHIPIA 1
#define SDL_TIMER_PHIPIA 1
#define SDL_VIDEO_DRIVER_PHIPIA 1
#define SDL_FILESYSTEM_PHIPIA 1
#define SDL_LOCALE_DUMMY 1
#define SDL_MISC_DUMMY 1

#define SDL_VIDEO_RENDER_SW 1
#define SDL_DYNAMIC_API 0

#endif /* SDL_config_phipia_h_ */
