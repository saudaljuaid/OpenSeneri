/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_CAMERA_H
#define PHIPIA_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAMERA_MAX_WIDTH 640U
#define CAMERA_MAX_HEIGHT 480U

enum camera_status {
    CAMERA_STATUS_OK = 0,
    CAMERA_STATUS_NULL_ARGUMENT,
    CAMERA_STATUS_BAD_GEOMETRY,
    CAMERA_STATUS_BAD_STRIDE,
    CAMERA_STATUS_NO_DEVICE,
    CAMERA_STATUS_BUFFER_TOO_SMALL,
    CAMERA_STATUS_FRAME_BUSY,
    CAMERA_STATUS_COUNT
};

struct camera_frame_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t timestamp_ns;
    uint64_t generation;
    uint64_t dropped_frames;
    bool connected;
};

/* Initialize the bounded RGB888 frame broker. A UVC or platform camera driver
 * publishes complete frames here; the desktop remains transport-agnostic. */
void camera_initialize(void);

/* Publish one complete top-down RGB888 frame. The bytes are copied before this
 * function returns, so a DMA transport may immediately recycle its buffer. */
enum camera_status camera_publish_rgb888(
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint64_t timestamp_ns
);

/* Mark the provider unavailable without exposing the previous frame as live. */
void camera_disconnect(void);

/* Snapshot the latest complete frame into the caller's packed framebuffer
 * format, resampling to the requested dimensions with bounded nearest lookup. */
enum camera_status camera_snapshot(
    uint32_t *out,
    size_t out_pixels,
    uint32_t out_width,
    uint32_t out_height,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    struct camera_frame_info *info
);

struct camera_frame_info camera_get_info(void);
bool camera_self_test(void);
const char *camera_status_string(enum camera_status status);

#endif
