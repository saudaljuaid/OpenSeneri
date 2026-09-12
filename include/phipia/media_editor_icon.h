/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_MEDIA_EDITOR_ICON_H
#define PHIPIA_MEDIA_EDITOR_ICON_H

#include <stddef.h>
#include <stdint.h>

/* Uses the checked SRL decoder and status values declared by logo.h. */
int32_t phipia_media_editor_icon_geometry(uint32_t *width, uint32_t *height);
int32_t phipia_media_editor_icon_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_media_editor_icon_decode_alpha(uint8_t *out, size_t out_pixels);

int32_t phipia_settings_icon_geometry(uint32_t *width, uint32_t *height);
int32_t phipia_settings_icon_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_settings_icon_decode_alpha(uint8_t *out, size_t out_pixels);

int32_t phipia_files_icon_geometry(uint32_t *width, uint32_t *height);
int32_t phipia_files_icon_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_files_icon_decode_alpha(uint8_t *out, size_t out_pixels);

int32_t phipia_terminal_icon_geometry(uint32_t *width, uint32_t *height);
int32_t phipia_terminal_icon_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_terminal_icon_decode_alpha(uint8_t *out, size_t out_pixels);

int32_t phipia_settings_category_icons_geometry(
    uint32_t *width,
    uint32_t *height
);
int32_t phipia_settings_category_icons_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_settings_category_icons_decode_alpha(
    uint8_t *out,
    size_t out_pixels
);

int32_t phipia_camera_icon_geometry(uint32_t *width, uint32_t *height);
int32_t phipia_camera_icon_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_camera_icon_decode_alpha(uint8_t *out, size_t out_pixels);

int32_t phipia_canvas_icon_geometry(uint32_t *width, uint32_t *height);
int32_t phipia_canvas_icon_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_canvas_icon_decode_alpha(uint8_t *out, size_t out_pixels);

int32_t phipia_store_icon_geometry(uint32_t *width, uint32_t *height);
int32_t phipia_store_icon_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_store_icon_decode_alpha(uint8_t *out, size_t out_pixels);

int32_t phipia_store_ui_icons_geometry(uint32_t *width, uint32_t *height);
int32_t phipia_store_ui_icons_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t phipia_store_ui_icons_decode_alpha(
    uint8_t *out,
    size_t out_pixels
);

#endif
