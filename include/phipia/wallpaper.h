/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_WALLPAPER_H
#define PHIPIA_WALLPAPER_H

#include <stddef.h>
#include <stdint.h>

enum wallpaper_status {
    WALLPAPER_STATUS_OK = 0,
    WALLPAPER_STATUS_NULL_ARGUMENT = 1,
    WALLPAPER_STATUS_BAD_HEADER = 2,
    WALLPAPER_STATUS_BAD_GEOMETRY = 3,
    WALLPAPER_STATUS_BAD_PALETTE = 4,
    WALLPAPER_STATUS_BUFFER_TOO_SMALL = 5,
    WALLPAPER_STATUS_BAD_FRAME = 6,
    WALLPAPER_STATUS_BAD_OUTPUT = 7
};

int32_t phipia_wallpaper_self_test(void);
size_t phipia_wallpaper_size(void);
int32_t phipia_wallpaper_geometry(
    uint32_t *width,
    uint32_t *height,
    uint32_t *frames
);
int32_t phipia_wallpaper_decode(
    uint32_t frame,
    uint32_t *out,
    size_t out_pixels,
    uint32_t out_width,
    uint32_t out_height,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift
);

#endif
