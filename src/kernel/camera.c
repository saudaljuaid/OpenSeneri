/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/camera.h>
#include <phipia/cpu.h>

#define CAMERA_BUFFER_BYTES \
    ((size_t)CAMERA_MAX_WIDTH * CAMERA_MAX_HEIGHT * 3U)
#define CAMERA_READER_NONE UINT8_MAX

struct camera_buffer {
    uint8_t pixels[CAMERA_BUFFER_BYTES];
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t timestamp_ns;
    uint64_t generation;
};

static struct camera_buffer buffers[2U];
static volatile uint8_t active_buffer;
static volatile uint8_t reader_buffer;
static volatile bool connected;
static volatile uint64_t generation;
static volatile uint64_t dropped_frames;

static void set_reader(uint8_t index)
{
    const bool enabled = cpu_interrupts_enabled();

    if (enabled) {
        cpu_interrupt_disable();
    }
    reader_buffer = index;
    if (enabled) {
        cpu_interrupt_enable();
    }
}

void camera_initialize(void)
{
    active_buffer = 0U;
    reader_buffer = CAMERA_READER_NONE;
    connected = false;
    generation = 0U;
    dropped_frames = 0U;
    for (size_t index = 0U; index < 2U; ++index) {
        buffers[index].width = 0U;
        buffers[index].height = 0U;
        buffers[index].stride = 0U;
        buffers[index].timestamp_ns = 0U;
        buffers[index].generation = 0U;
    }
}

enum camera_status camera_publish_rgb888(
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint64_t timestamp_ns
)
{
    uint8_t destination;
    size_t row_bytes;

    if (pixels == NULL) {
        return CAMERA_STATUS_NULL_ARGUMENT;
    }
    if (width == 0U || height == 0U || width > CAMERA_MAX_WIDTH ||
            height > CAMERA_MAX_HEIGHT) {
        return CAMERA_STATUS_BAD_GEOMETRY;
    }
    row_bytes = (size_t)width * 3U;
    if (stride < row_bytes) {
        return CAMERA_STATUS_BAD_STRIDE;
    }
    destination = (uint8_t)(active_buffer ^ 1U);
    if (reader_buffer == destination) {
        ++dropped_frames;
        return CAMERA_STATUS_FRAME_BUSY;
    }
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *source = pixels + (size_t)y * stride;
        uint8_t *target = buffers[destination].pixels +
            (size_t)y * row_bytes;

        for (size_t byte = 0U; byte < row_bytes; ++byte) {
            target[byte] = source[byte];
        }
    }
    buffers[destination].width = width;
    buffers[destination].height = height;
    buffers[destination].stride = (uint32_t)row_bytes;
    buffers[destination].timestamp_ns = timestamp_ns;
    buffers[destination].generation = generation + 1U;

    const bool enabled = cpu_interrupts_enabled();
    if (enabled) {
        cpu_interrupt_disable();
    }
    ++generation;
    buffers[destination].generation = generation;
    active_buffer = destination;
    connected = true;
    if (enabled) {
        cpu_interrupt_enable();
    }
    return CAMERA_STATUS_OK;
}

void camera_disconnect(void)
{
    const bool enabled = cpu_interrupts_enabled();

    if (enabled) {
        cpu_interrupt_disable();
    }
    connected = false;
    ++generation;
    if (enabled) {
        cpu_interrupt_enable();
    }
}

enum camera_status camera_snapshot(
    uint32_t *out,
    size_t out_pixels,
    uint32_t out_width,
    uint32_t out_height,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    struct camera_frame_info *info
)
{
    uint8_t source_index;
    const struct camera_buffer *source;

    if (out == NULL) {
        return CAMERA_STATUS_NULL_ARGUMENT;
    }
    if (out_width == 0U || out_height == 0U ||
            out_width > CAMERA_MAX_WIDTH * 2U ||
            out_height > CAMERA_MAX_HEIGHT * 2U) {
        return CAMERA_STATUS_BAD_GEOMETRY;
    }
    if (out_pixels < (size_t)out_width * out_height) {
        return CAMERA_STATUS_BUFFER_TOO_SMALL;
    }
    if (!connected) {
        return CAMERA_STATUS_NO_DEVICE;
    }
    source_index = active_buffer;
    set_reader(source_index);
    source = &buffers[source_index];
    for (uint32_t y = 0U; y < out_height; ++y) {
        const uint32_t source_y = y * source->height / out_height;
        for (uint32_t x = 0U; x < out_width; ++x) {
            const uint32_t source_x = x * source->width / out_width;
            const size_t offset = (size_t)source_y * source->stride +
                (size_t)source_x * 3U;
            out[(size_t)y * out_width + x] =
                ((uint32_t)source->pixels[offset] << red_shift) |
                ((uint32_t)source->pixels[offset + 1U] << green_shift) |
                ((uint32_t)source->pixels[offset + 2U] << blue_shift);
        }
    }
    if (info != NULL) {
        *info = (struct camera_frame_info){
            .width = source->width,
            .height = source->height,
            .stride = source->stride,
            .timestamp_ns = source->timestamp_ns,
            .generation = source->generation,
            .dropped_frames = dropped_frames,
            .connected = true
        };
    }
    set_reader(CAMERA_READER_NONE);
    return CAMERA_STATUS_OK;
}

struct camera_frame_info camera_get_info(void)
{
    const struct camera_buffer *source = &buffers[active_buffer];

    return (struct camera_frame_info){
        .width = connected ? source->width : 0U,
        .height = connected ? source->height : 0U,
        .stride = connected ? source->stride : 0U,
        .timestamp_ns = connected ? source->timestamp_ns : 0U,
        .generation = generation,
        .dropped_frames = dropped_frames,
        .connected = connected
    };
}

bool camera_self_test(void)
{
    static const uint8_t pixel[3U] = { 0x12U, 0x34U, 0x56U };
    uint32_t output = 0U;
    struct camera_frame_info info;
    bool passed;

    camera_initialize();
    passed = camera_publish_rgb888(NULL, 1U, 1U, 3U, 1U) ==
            CAMERA_STATUS_NULL_ARGUMENT &&
        camera_publish_rgb888(pixel, 0U, 1U, 3U, 1U) ==
            CAMERA_STATUS_BAD_GEOMETRY &&
        camera_publish_rgb888(pixel, 1U, 1U, 2U, 1U) ==
            CAMERA_STATUS_BAD_STRIDE &&
        camera_publish_rgb888(pixel, 1U, 1U, 3U, 42U) ==
            CAMERA_STATUS_OK &&
        camera_snapshot(&output, 1U, 1U, 1U, 16U, 8U, 0U, &info) ==
            CAMERA_STATUS_OK &&
        output == UINT32_C(0x00123456) && info.connected &&
        info.width == 1U && info.height == 1U &&
        info.timestamp_ns == 42U && info.generation == 1U;
    camera_initialize();
    return passed && camera_snapshot(&output, 1U, 1U, 1U,
        16U, 8U, 0U, NULL) == CAMERA_STATUS_NO_DEVICE;
}

const char *camera_status_string(enum camera_status status)
{
    static const char *const messages[] = {
        "ok", "null camera argument", "unsupported camera geometry",
        "invalid camera row stride", "no camera connected",
        "camera output buffer too small", "camera frame dropped while busy"
    };

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown camera status";
    }
    return messages[status];
}
