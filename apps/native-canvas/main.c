/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/runtime.h>
#include <phipia/window.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS_WIDTH UINT32_C(420)
#define CANVAS_HEIGHT UINT32_C(250)
#define FRAME_NS UINT64_C(75000000)
#define RUN_NS UINT64_C(6000000000)
#define TOOLBAR_WIDTH UINT32_C(44)
#define PALETTE_X0 UINT32_C(344)
#define STATUS_Y UINT32_C(226)
#define ARTBOARD_X UINT32_C(50)
#define ARTBOARD_Y UINT32_C(7)
#define ARTBOARD_WIDTH UINT32_C(287)
#define ARTBOARD_HEIGHT UINT32_C(213)
#define PALETTE_RADIUS INT32_C(9)
#define ERASER_RADIUS INT32_C(6)
#define TOOL_COUNT 5U
#define FONT_HEADER_BYTES 24U
#define FONT_MAX_BYTES 32768U
#define TOOL_ICON_HEADER_BYTES 16U
#define TOOL_ICON_WIDTH 24U
#define TOOL_ICON_HEIGHT 24U
#define TOOL_ICON_BYTES (TOOL_ICON_WIDTH * TOOL_ICON_HEIGHT)
#define TOOL_ICON_FILE_BYTES (TOOL_ICON_HEADER_BYTES + TOOL_COUNT * \
    TOOL_ICON_BYTES)

#define COLOR_BACKGROUND UINT32_C(0xC6CBD0)
#define COLOR_PANEL UINT32_C(0xE2E4E6)
#define COLOR_PANEL_LIGHT UINT32_C(0xF7F8F8)
#define COLOR_PANEL_DARK UINT32_C(0x858B90)
#define COLOR_WORKSPACE UINT32_C(0xBFC3C6)
#define COLOR_PAPER UINT32_C(0xFFFFFF)
#define COLOR_INK UINT32_C(0x202326)
#define COLOR_MUTED UINT32_C(0x687078)
#define COLOR_TEAL UINT32_C(0x26BFA4)
#define COLOR_CORAL UINT32_C(0xE95A55)
#define COLOR_VIOLET UINT32_C(0x7659D7)
#define COLOR_GOLD UINT32_C(0xF2B84B)
#define COLOR_BLUE UINT32_C(0x3D7DE0)

struct canvas {
    uint32_t *pixels;
    uint32_t stride;
};

enum drawing_tool {
    TOOL_BRUSH = 0,
    TOOL_LINE,
    TOOL_RECTANGLE,
    TOOL_ELLIPSE,
    TOOL_ERASER
};

static const uint32_t palette_colors[] = {
    COLOR_INK, COLOR_BLUE, COLOR_VIOLET, COLOR_CORAL, COLOR_TEAL, COLOR_GOLD
};

static const int32_t brush_sizes[] = {2, 4, 6};

static uint8_t *inter_font;
static size_t inter_font_size;
static uint8_t *tool_icons;

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8U |
        (uint32_t)bytes[2] << 16U | (uint32_t)bytes[3] << 24U;
}

static bool read_exact(phipia_handle_t handle, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    size_t completed = 0U;

    while (completed < length) {
        const long result = phipia_file_read(handle, bytes + completed,
            length - completed);

        if (result <= 0) {
            return false;
        }
        completed += (size_t)result;
    }
    return true;
}

static bool load_inter_font(void)
{
    uint8_t header[FONT_HEADER_BYTES];
    uint8_t extra;
    long opened = phipia_file_open(PHIPIA_VOLUME_SYSTEM, "FONT.SUF",
        PHIPIA_OPEN_READ);
    size_t total;

    if (opened < 0 || !read_exact((phipia_handle_t)opened, header,
            sizeof(header)) || memcmp(header, "SUF2", 4U) != 0 ||
            header[4] != 2U || header[5] != FONT_HEADER_BYTES ||
            header[6] != 16U || header[7] != 19U || header[11] != 16U ||
            read_u32(header + 12U) != 0x20U ||
            read_u32(header + 16U) != 95U ||
            read_u32(header + 20U) > FONT_MAX_BYTES - FONT_HEADER_BYTES) {
        if (opened >= 0) {
            (void)phipia_handle_close((phipia_handle_t)opened);
        }
        return false;
    }
    total = FONT_HEADER_BYTES + (size_t)read_u32(header + 20U);
    inter_font = malloc(total);
    if (inter_font == NULL) {
        (void)phipia_handle_close((phipia_handle_t)opened);
        return false;
    }
    memcpy(inter_font, header, sizeof(header));
    if (!read_exact((phipia_handle_t)opened, inter_font + sizeof(header),
            total - sizeof(header)) ||
            phipia_file_read((phipia_handle_t)opened, &extra, 1U) != 0 ||
            phipia_handle_close((phipia_handle_t)opened) != 0) {
        free(inter_font);
        inter_font = NULL;
        return false;
    }
    inter_font_size = total;
    return true;
}

static uint32_t crc32(const uint8_t *bytes, size_t length)
{
    uint32_t value = UINT32_C(0xFFFFFFFF);

    for (size_t index = 0U; index < length; ++index) {
        value ^= bytes[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            value = value >> 1U ^ (UINT32_C(0xEDB88320) &
                (uint32_t)-(int32_t)(value & 1U));
        }
    }
    return ~value;
}

static bool load_tool_icons(void)
{
    uint8_t extra;
    long opened = phipia_file_open(PHIPIA_VOLUME_SYSTEM, "TOOLS.A8",
        PHIPIA_OPEN_READ);

    tool_icons = malloc(TOOL_ICON_FILE_BYTES);
    if (opened < 0 || tool_icons == NULL ||
            !read_exact((phipia_handle_t)opened, tool_icons,
                TOOL_ICON_FILE_BYTES) ||
            phipia_file_read((phipia_handle_t)opened, &extra, 1U) != 0 ||
            memcmp(tool_icons, "SCI1", 4U) != 0 || tool_icons[4] != 1U ||
            tool_icons[5] != TOOL_ICON_WIDTH ||
            tool_icons[6] != TOOL_ICON_HEIGHT || tool_icons[7] != TOOL_COUNT ||
            read_u32(tool_icons + 8U) != TOOL_COUNT * TOOL_ICON_BYTES ||
            crc32(tool_icons + TOOL_ICON_HEADER_BYTES,
                TOOL_COUNT * TOOL_ICON_BYTES) != read_u32(tool_icons + 12U)) {
        if (opened >= 0) {
            (void)phipia_handle_close((phipia_handle_t)opened);
        }
        free(tool_icons);
        tool_icons = NULL;
        return false;
    }
    if (phipia_handle_close((phipia_handle_t)opened) != 0) {
        free(tool_icons);
        tool_icons = NULL;
        return false;
    }
    return true;
}

static void fill_rect(struct canvas *canvas, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t color)
{
    for (uint32_t row = y; row < y + height && row < CANVAS_HEIGHT; ++row) {
        for (uint32_t column = x;
             column < x + width && column < CANVAS_WIDTH; ++column) {
            canvas->pixels[(size_t)row * canvas->stride + column] = color;
        }
    }
}

static void put_pixel(struct canvas *canvas, int32_t x, int32_t y,
    uint32_t color)
{
    if (x >= 0 && y >= 0 && (uint32_t)x < CANVAS_WIDTH &&
        (uint32_t)y < CANVAS_HEIGHT) {
        canvas->pixels[(size_t)(uint32_t)y * canvas->stride +
            (uint32_t)x] = color;
    }
}

static void stroke_rect(struct canvas *canvas, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t thickness, uint32_t color)
{
    if (width == 0U || height == 0U || thickness == 0U) {
        return;
    }
    fill_rect(canvas, x, y, width, thickness, color);
    fill_rect(canvas, x, y + height - thickness, width, thickness, color);
    fill_rect(canvas, x, y, thickness, height, color);
    fill_rect(canvas, x + width - thickness, y, thickness, height, color);
}

static void fill_circle(struct canvas *canvas, int32_t center_x,
    int32_t center_y, int32_t radius, uint32_t color)
{
    const int32_t squared = radius * radius;

    for (int32_t y = -radius; y <= radius; ++y) {
        for (int32_t x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= squared) {
                put_pixel(canvas, center_x + x, center_y + y, color);
            }
        }
    }
}

static void draw_line(struct canvas *canvas, int32_t x0, int32_t y0,
    int32_t x1, int32_t y1, int32_t thickness, uint32_t color)
{
    const int32_t delta_x = x0 < x1 ? x1 - x0 : x0 - x1;
    const int32_t step_x = x0 < x1 ? 1 : -1;
    const int32_t delta_y = -(y0 < y1 ? y1 - y0 : y0 - y1);
    const int32_t step_y = y0 < y1 ? 1 : -1;
    int32_t error = delta_x + delta_y;

    for (;;) {
        const int32_t radius = thickness / 2;

        for (int32_t y = -radius; y <= radius; ++y) {
            for (int32_t x = -radius; x <= radius; ++x) {
                put_pixel(canvas, x0 + x, y0 + y, color);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int32_t doubled = error * 2;

        if (doubled >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (doubled <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

static void draw_ellipse(struct canvas *canvas, int32_t left, int32_t top,
    int32_t right, int32_t bottom, uint32_t color)
{
    int32_t radius_x;
    int32_t radius_y;
    int32_t center_x;
    int32_t center_y;
    int64_t target;
    int64_t tolerance;

    if (left > right) {
        const int32_t temporary = left;
        left = right;
        right = temporary;
    }
    if (top > bottom) {
        const int32_t temporary = top;
        top = bottom;
        bottom = temporary;
    }
    radius_x = (right - left) / 2;
    radius_y = (bottom - top) / 2;
    center_x = left + radius_x;
    center_y = top + radius_y;
    if (radius_x < 2 || radius_y < 2) {
        draw_line(canvas, left, top, right, bottom, 2, color);
        return;
    }
    target = (int64_t)radius_x * radius_x * radius_y * radius_y;
    tolerance = ((int64_t)radius_x * radius_x +
        (int64_t)radius_y * radius_y) * 2;
    for (int32_t y = top; y <= bottom; ++y) {
        for (int32_t x = left; x <= right; ++x) {
            const int64_t delta_x = x - center_x;
            const int64_t delta_y = y - center_y;
            const int64_t distance = delta_x * delta_x * radius_y * radius_y +
                delta_y * delta_y * radius_x * radius_x;
            const int64_t difference = distance > target ?
                distance - target : target - distance;

            if (difference <= tolerance) {
                put_pixel(canvas, x, y, color);
            }
        }
    }
}

static uint32_t blend_color(uint32_t background, uint32_t foreground,
    uint8_t alpha)
{
    const uint32_t inverse = 255U - alpha;
    const uint32_t red = (((foreground >> 16U) & 0xFFU) * alpha +
        ((background >> 16U) & 0xFFU) * inverse + 127U) / 255U;
    const uint32_t green = (((foreground >> 8U) & 0xFFU) * alpha +
        ((background >> 8U) & 0xFFU) * inverse + 127U) / 255U;
    const uint32_t blue = ((foreground & 0xFFU) * alpha +
        (background & 0xFFU) * inverse + 127U) / 255U;

    return red << 16U | green << 8U | blue;
}

static void blend_pixel(struct canvas *canvas, int32_t x, int32_t y,
    uint32_t color, uint8_t alpha)
{
    size_t pixel;

    if (alpha == 0U || x < 0 || y < 0 || (uint32_t)x >= CANVAS_WIDTH ||
            (uint32_t)y >= CANVAS_HEIGHT) {
        return;
    }
    pixel = (size_t)(uint32_t)y * canvas->stride + (uint32_t)x;
    canvas->pixels[pixel] = blend_color(canvas->pixels[pixel], color, alpha);
}

static void fill_aa_circle(struct canvas *canvas, int32_t center_x,
    int32_t center_y, int32_t radius, uint32_t color)
{
    const int32_t radius_scaled = radius * 8;
    const int32_t squared = radius_scaled * radius_scaled;

    for (int32_t y = -radius - 1; y <= radius + 1; ++y) {
        for (int32_t x = -radius - 1; x <= radius + 1; ++x) {
            uint32_t covered = 0U;

            for (int32_t sample_y = 0; sample_y < 4; ++sample_y) {
                for (int32_t sample_x = 0; sample_x < 4; ++sample_x) {
                    const int32_t dx = x * 8 + sample_x * 2 + 1;
                    const int32_t dy = y * 8 + sample_y * 2 + 1;

                    if (dx * dx + dy * dy <= squared) {
                        ++covered;
                    }
                }
            }
            blend_pixel(canvas, center_x + x, center_y + y, color,
                (uint8_t)(covered * 255U / 16U));
        }
    }
}

static void draw_text(struct canvas *canvas, uint32_t x, uint32_t y,
    const char *text, uint32_t color)
{
    const size_t glyph_stride = 1U + 16U * 19U;

    if (inter_font == NULL || inter_font_size < FONT_HEADER_BYTES) {
        return;
    }
    while (*text != '\0') {
        uint8_t code = (uint8_t)*text++;
        size_t offset;
        uint8_t advance;

        if (code < 0x20U || code >= 0x7FU) {
            code = (uint8_t)'?';
        }
        offset = FONT_HEADER_BYTES + (size_t)(code - 0x20U) * glyph_stride;
        if (offset + glyph_stride > inter_font_size) {
            return;
        }
        advance = inter_font[offset];
        for (uint32_t row = 0U; row < 19U; ++row) {
            for (uint32_t column = 0U; column < 16U; ++column) {
                const uint8_t alpha = inter_font[offset + 1U +
                    (size_t)row * 16U + column];
                const uint32_t target_x = x + column;
                const uint32_t target_y = y + row;

                if (alpha != 0U && target_x < CANVAS_WIDTH &&
                        target_y < CANVAS_HEIGHT) {
                    const size_t pixel = (size_t)target_y * canvas->stride +
                        target_x;

                    canvas->pixels[pixel] = blend_color(canvas->pixels[pixel],
                        color, alpha);
                }
            }
        }
        x += advance;
    }
}

static void draw_palette(struct canvas *canvas, size_t selected,
    size_t selected_size)
{
    fill_rect(canvas, PALETTE_X0, 0U, CANVAS_WIDTH - PALETTE_X0,
        STATUS_Y, COLOR_PANEL);
    fill_rect(canvas, PALETTE_X0, 0U, 1U, STATUS_Y, COLOR_PANEL_DARK);
    fill_rect(canvas, PALETTE_X0 + 1U, 0U, 1U, STATUS_Y,
        COLOR_PANEL_LIGHT);
    for (size_t index = 0U;
         index < sizeof(palette_colors) / sizeof(palette_colors[0]);
         ++index) {
        const int32_t x = 363 + (int32_t)(index % 2U) * 34;
        const int32_t y = 25 + (int32_t)(index / 2U) * 33;

        if (index == selected) {
            fill_aa_circle(canvas, x, y, PALETTE_RADIUS + 3, COLOR_INK);
        }
        fill_aa_circle(canvas, x, y, PALETTE_RADIUS,
            palette_colors[index]);
    }
    fill_rect(canvas, PALETTE_X0 + 8U, 115U,
        CANVAS_WIDTH - PALETTE_X0 - 16U, 1U, COLOR_PANEL_DARK);
    fill_rect(canvas, PALETTE_X0 + 8U, 116U,
        CANVAS_WIDTH - PALETTE_X0 - 16U, 1U, COLOR_PANEL_LIGHT);
    draw_text(canvas, PALETTE_X0 + 18U, 119U, "Brush", COLOR_MUTED);
    for (size_t index = 0U; index < sizeof(brush_sizes) /
            sizeof(brush_sizes[0]); ++index) {
        const uint32_t x = PALETTE_X0 + 7U + (uint32_t)index * 23U;
        const int32_t center_x = (int32_t)x + 9;
        const int32_t center_y = 166;

        fill_rect(canvas, x, 148U, 19U, 36U,
            index == selected_size ? UINT32_C(0xB8BDC1) : COLOR_PANEL);
        if (index == selected_size) {
            stroke_rect(canvas, x, 148U, 19U, 36U, 1U, COLOR_PANEL_DARK);
        }
        fill_aa_circle(canvas, center_x, center_y, brush_sizes[index],
            COLOR_INK);
    }
}

static void draw_tool_icon(struct canvas *canvas, enum drawing_tool tool,
    uint32_t y, bool selected)
{
    const uint32_t icon = selected ? COLOR_INK : COLOR_MUTED;
    const size_t source = TOOL_ICON_HEADER_BYTES +
        (size_t)tool * TOOL_ICON_BYTES;

    fill_rect(canvas, 6U, y, 32U, 32U,
        selected ? UINT32_C(0xC3C7CA) : COLOR_PANEL);
    if (selected) {
        fill_rect(canvas, 6U, y, 32U, 1U, COLOR_PANEL_DARK);
        fill_rect(canvas, 6U, y, 1U, 32U, COLOR_PANEL_DARK);
        fill_rect(canvas, 7U, y + 31U, 31U, 1U, COLOR_PANEL_LIGHT);
        fill_rect(canvas, 37U, y + 1U, 1U, 31U, COLOR_PANEL_LIGHT);
    }
    if (tool_icons == NULL || (uint32_t)tool >= TOOL_COUNT) {
        return;
    }
    for (uint32_t row = 0U; row < TOOL_ICON_HEIGHT; ++row) {
        for (uint32_t column = 0U; column < TOOL_ICON_WIDTH; ++column) {
            blend_pixel(canvas, 10 + (int32_t)column,
                (int32_t)y + 4 + (int32_t)row, icon,
                tool_icons[source + (size_t)row * TOOL_ICON_WIDTH + column]);
        }
    }
}

static void draw_toolbar(struct canvas *canvas, enum drawing_tool selected)
{
    fill_rect(canvas, 0U, 0U, TOOLBAR_WIDTH, STATUS_Y, COLOR_PANEL);
    fill_rect(canvas, TOOLBAR_WIDTH - 2U, 0U, 1U, STATUS_Y,
        COLOR_PANEL_LIGHT);
    fill_rect(canvas, TOOLBAR_WIDTH - 1U, 0U, 1U, STATUS_Y,
        COLOR_PANEL_DARK);
    for (uint32_t index = 0U; index < TOOL_COUNT; ++index) {
        draw_tool_icon(canvas, (enum drawing_tool)index, 8U + index * 43U,
            selected == (enum drawing_tool)index);
    }
}

static void draw_workspace(struct canvas *canvas, size_t selected_color,
    size_t selected_size, enum drawing_tool selected_tool, int focused)
{

    fill_rect(canvas, 0U, 0U, CANVAS_WIDTH, CANVAS_HEIGHT,
        COLOR_BACKGROUND);
    fill_rect(canvas, TOOLBAR_WIDTH, 0U, PALETTE_X0 - TOOLBAR_WIDTH,
        STATUS_Y, COLOR_WORKSPACE);
    fill_rect(canvas, ARTBOARD_X + 3U, ARTBOARD_Y + 4U,
        ARTBOARD_WIDTH, ARTBOARD_HEIGHT, UINT32_C(0x898D90));
    fill_rect(canvas, ARTBOARD_X, ARTBOARD_Y, ARTBOARD_WIDTH,
        ARTBOARD_HEIGHT, COLOR_PAPER);
    draw_toolbar(canvas, selected_tool);
    draw_palette(canvas, selected_color, selected_size);
    fill_rect(canvas, 0U, STATUS_Y, CANVAS_WIDTH,
        CANVAS_HEIGHT - STATUS_Y, COLOR_PANEL);
    fill_rect(canvas, 0U, STATUS_Y, CANVAS_WIDTH, 1U, COLOR_PANEL_DARK);
    fill_rect(canvas, 0U, STATUS_Y + 1U, CANVAS_WIDTH, 1U,
        COLOR_PANEL_LIGHT);
    draw_text(canvas, 10U, STATUS_Y + 2U, "100%", COLOR_MUTED);
    draw_text(canvas, 184U, STATUS_Y + 2U, "Untitled", COLOR_MUTED);
    stroke_rect(canvas, ARTBOARD_X - 1U, ARTBOARD_Y - 1U,
        ARTBOARD_WIDTH + 2U, ARTBOARD_HEIGHT + 2U, 1U,
        focused != 0 ? UINT32_C(0x666A6D) : UINT32_C(0x999DA0));
}

static bool point_in_artboard(int32_t x, int32_t y)
{
    return x >= (int32_t)ARTBOARD_X && y >= (int32_t)ARTBOARD_Y &&
        x < (int32_t)(ARTBOARD_X + ARTBOARD_WIDTH) &&
        y < (int32_t)(ARTBOARD_Y + ARTBOARD_HEIGHT);
}

static size_t palette_at(int32_t x, int32_t y)
{
    for (size_t index = 0U;
         index < sizeof(palette_colors) / sizeof(palette_colors[0]);
         ++index) {
        const int32_t center_x = 363 + (int32_t)(index % 2U) * 34;
        const int32_t center_y = 25 + (int32_t)(index / 2U) * 33;
        const int32_t delta_x = x - center_x;
        const int32_t delta_y = y - center_y;
        const int32_t radius = PALETTE_RADIUS + 5;

        if (delta_x * delta_x + delta_y * delta_y <= radius * radius) {
            return index;
        }
    }
    return SIZE_MAX;
}

static size_t size_at(int32_t x, int32_t y)
{
    if (y < 148 || y >= 184) {
        return SIZE_MAX;
    }
    for (size_t index = 0U; index < sizeof(brush_sizes) /
            sizeof(brush_sizes[0]); ++index) {
        const int32_t left = (int32_t)PALETTE_X0 + 7 +
            (int32_t)index * 23;

        if (x >= left && x < left + 19) {
            return index;
        }
    }
    return SIZE_MAX;
}

static enum drawing_tool tool_at(int32_t x, int32_t y)
{
    if (x < 6 || x >= 38) {
        return TOOL_COUNT;
    }
    for (uint32_t index = 0U; index < TOOL_COUNT; ++index) {
        const int32_t top = 8 + (int32_t)index * 43;

        if (y >= top && y < top + 32) {
            return (enum drawing_tool)index;
        }
    }
    return TOOL_COUNT;
}

static int32_t clamp_artboard_x(int32_t x)
{
    if (x < (int32_t)ARTBOARD_X) {
        return (int32_t)ARTBOARD_X;
    }
    if (x >= (int32_t)(ARTBOARD_X + ARTBOARD_WIDTH)) {
        return (int32_t)(ARTBOARD_X + ARTBOARD_WIDTH - 1U);
    }
    return x;
}

static int32_t clamp_artboard_y(int32_t y)
{
    if (y < (int32_t)ARTBOARD_Y) {
        return (int32_t)ARTBOARD_Y;
    }
    if (y >= (int32_t)(ARTBOARD_Y + ARTBOARD_HEIGHT)) {
        return (int32_t)(ARTBOARD_Y + ARTBOARD_HEIGHT - 1U);
    }
    return y;
}

static struct phipia_rect brush_damage(int32_t from_x, int32_t from_y,
    int32_t to_x, int32_t to_y, int32_t radius)
{
    int32_t left = from_x < to_x ? from_x : to_x;
    int32_t top = from_y < to_y ? from_y : to_y;
    int32_t right = from_x > to_x ? from_x : to_x;
    int32_t bottom = from_y > to_y ? from_y : to_y;

    left = clamp_artboard_x(left - radius);
    top = clamp_artboard_y(top - radius);
    right = clamp_artboard_x(right + radius);
    bottom = clamp_artboard_y(bottom + radius);
    return (struct phipia_rect){
        (uint32_t)left, (uint32_t)top,
        (uint32_t)(right - left + 1), (uint32_t)(bottom - top + 1)
    };
}

static struct phipia_rect shape_damage(int32_t from_x, int32_t from_y,
    int32_t to_x, int32_t to_y)
{
    int32_t left = from_x < to_x ? from_x : to_x;
    int32_t top = from_y < to_y ? from_y : to_y;
    int32_t right = from_x > to_x ? from_x : to_x;
    int32_t bottom = from_y > to_y ? from_y : to_y;

    left = clamp_artboard_x(left - 2);
    top = clamp_artboard_y(top - 2);
    right = clamp_artboard_x(right + 2);
    bottom = clamp_artboard_y(bottom + 2);
    return (struct phipia_rect){
        (uint32_t)left, (uint32_t)top,
        (uint32_t)(right - left + 1), (uint32_t)(bottom - top + 1)
    };
}

static void draw_shape(struct canvas *canvas, enum drawing_tool tool,
    int32_t from_x, int32_t from_y, int32_t to_x, int32_t to_y,
    uint32_t color)
{
    int32_t left = from_x < to_x ? from_x : to_x;
    int32_t top = from_y < to_y ? from_y : to_y;
    int32_t right = from_x > to_x ? from_x : to_x;
    int32_t bottom = from_y > to_y ? from_y : to_y;

    if (tool == TOOL_LINE) {
        draw_line(canvas, from_x, from_y, to_x, to_y, 2, color);
    } else if (tool == TOOL_RECTANGLE) {
        const uint32_t width = (uint32_t)(right - left + 1);
        const uint32_t height = (uint32_t)(bottom - top + 1);

        if (width < 2U || height < 2U) {
            draw_line(canvas, from_x, from_y, to_x, to_y, 2, color);
        } else {
            stroke_rect(canvas, (uint32_t)left, (uint32_t)top,
                width, height, 2U, color);
        }
    } else if (tool == TOOL_ELLIPSE) {
        draw_ellipse(canvas, left, top, right, bottom, color);
    }
}

static int present(phipia_handle_t window, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
    const struct phipia_rect damage = {x, y, width, height};
    return phipia_surface_present(window, &damage, 1U) < 0 ? -1 : 0;
}

int main(int argc, char **argv, char **environment)
{
    struct phipia_window_create_response response = {0};
    struct canvas canvas;
    uint64_t started;
    uint32_t partial_presents = 0U;
    uint32_t focus_events = 0U;
    uint32_t key_events = 0U;
    uint32_t pointer_events = 0U;
    uint32_t present_samples = 0U;
    uint32_t maximum_damage_pixels = 0U;
    uint32_t stroke_segments = 0U;
    uint32_t color_changes = 0U;
    size_t selected_color = 2U;
    size_t selected_size = 1U;
    enum drawing_tool selected_tool = TOOL_BRUSH;
    uint64_t present_elapsed = 0U;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t previous_x = 0;
    int32_t previous_y = 0;
    int focused = 0;
    int drawing = 0;
    int proof_mode = argc > 1 && strcmp(argv[1], "multi-window-proof") == 0;
    int running = 1;

    (void)environment;
    if (!load_inter_font() || !load_tool_icons()) {
        puts("PHIPIA CANVAS RESOURCE FAIL");
        return 19;
    }
    const int create_status = phipia_window_create("Untitled", CANVAS_WIDTH,
        CANVAS_HEIGHT, &response);

    if (create_status != 0 ||
        response.surface_address == 0U ||
        response.stride_bytes != CANVAS_WIDTH * sizeof(uint32_t)) {
        printf("PHIPIA CANVAS CREATE FAIL status=%d errno=%d surface=%llu "
            "stride=%u expected=%u\n", create_status, errno,
            (unsigned long long)response.surface_address,
            response.stride_bytes, CANVAS_WIDTH * (uint32_t)sizeof(uint32_t));
        return 20;
    }
    canvas.pixels = (uint32_t *)(uintptr_t)response.surface_address;
    canvas.stride = response.stride_bytes / sizeof(uint32_t);
    draw_workspace(&canvas, selected_color, selected_size, selected_tool,
        focused);
    if (present(response.window, 0U, 0U, CANVAS_WIDTH, CANVAS_HEIGHT) != 0) {
        return 21;
    }
    printf("PHIPIA CANVAS READY width=%u height=%u\n", response.width,
        response.height);
    started = phipia_monotonic_ns();
    while (running != 0 && (proof_mode == 0 ||
            phipia_monotonic_ns() - started < RUN_NS)) {
        const uint64_t now = phipia_monotonic_ns();
        struct phipia_event event;
        long wait_result;

        wait_result = phipia_event_wait(response.events, now + FRAME_NS);
        if (wait_result < 0 && wait_result != -PHIPIA_ETIMEDOUT) {
            return 23;
        }
        while (phipia_event_read(response.events, &event) > 0) {
            if (event.type == PHIPIA_EVENT_FOCUS) {
                ++focus_events;
                focused = event.value != 0U;
                stroke_rect(&canvas, ARTBOARD_X - 1U, ARTBOARD_Y - 1U,
                    ARTBOARD_WIDTH + 2U, ARTBOARD_HEIGHT + 2U, 1U,
                    focused != 0 ? UINT32_C(0x666A6D) :
                        UINT32_C(0x999DA0));
                if (present(response.window, ARTBOARD_X - 1U,
                        ARTBOARD_Y - 1U, ARTBOARD_WIDTH + 2U,
                        ARTBOARD_HEIGHT + 2U) != 0) {
                    return 24;
                }
                ++partial_presents;
            } else if (event.type == PHIPIA_EVENT_KEY &&
                event.value != PHIPIA_KEY_RELEASED) {
                ++key_events;
                selected_color = (selected_color + 1U) %
                    (sizeof(palette_colors) / sizeof(palette_colors[0]));
                ++color_changes;
                draw_palette(&canvas, selected_color, selected_size);
                if (present(response.window, PALETTE_X0, 0U,
                        CANVAS_WIDTH - PALETTE_X0, STATUS_Y) != 0) {
                    return 25;
                }
                ++partial_presents;
            } else if (event.type == PHIPIA_EVENT_POINTER_MOVE) {
                ++pointer_events;
                if (drawing != 0 && (selected_tool == TOOL_BRUSH ||
                        selected_tool == TOOL_ERASER)) {
                    const int32_t x = clamp_artboard_x(event.x);
                    const int32_t y = clamp_artboard_y(event.y);
                    const int32_t radius = selected_tool == TOOL_ERASER ?
                        ERASER_RADIUS : brush_sizes[selected_size];
                    const struct phipia_rect damage = brush_damage(previous_x,
                        previous_y, x, y, radius);
                    uint64_t present_started;

                    draw_line(&canvas, previous_x, previous_y, x, y,
                        radius * 2 + 1, selected_tool == TOOL_ERASER ?
                            COLOR_PAPER : palette_colors[selected_color]);
                    previous_x = x;
                    previous_y = y;
                    ++stroke_segments;
                    present_started = phipia_monotonic_ns();
                    if (present(response.window, damage.x, damage.y,
                            damage.width, damage.height) != 0) {
                        return 26;
                    }
                    present_elapsed += phipia_monotonic_ns() - present_started;
                    if (damage.width * damage.height > maximum_damage_pixels) {
                        maximum_damage_pixels = damage.width * damage.height;
                    }
                    ++present_samples;
                    ++partial_presents;
                }
            } else if (event.type == PHIPIA_EVENT_POINTER_BUTTON) {
                ++pointer_events;
                if (event.value != 0U) {
                    const enum drawing_tool tool = tool_at(event.x, event.y);
                    const size_t palette = palette_at(event.x, event.y);
                    const size_t size = size_at(event.x, event.y);

                    if ((uint32_t)tool < TOOL_COUNT) {
                        selected_tool = tool;
                        draw_toolbar(&canvas, selected_tool);
                        if (present(response.window, 0U, 0U, TOOLBAR_WIDTH,
                                STATUS_Y) != 0) {
                            return 26;
                        }
                        ++partial_presents;
                    } else if (palette != SIZE_MAX) {
                        selected_color = palette;
                        ++color_changes;
                        draw_palette(&canvas, selected_color, selected_size);
                        if (present(response.window, PALETTE_X0, 0U,
                                CANVAS_WIDTH - PALETTE_X0, STATUS_Y) != 0) {
                            return 26;
                        }
                        ++partial_presents;
                    } else if (size != SIZE_MAX) {
                        selected_size = size;
                        draw_palette(&canvas, selected_color, selected_size);
                        if (present(response.window, PALETTE_X0, 0U,
                                CANVAS_WIDTH - PALETTE_X0, STATUS_Y) != 0) {
                            return 26;
                        }
                        ++partial_presents;
                    } else if (point_in_artboard(event.x, event.y)) {
                        const int32_t radius = selected_tool == TOOL_ERASER ?
                            ERASER_RADIUS : brush_sizes[selected_size];

                        drawing = 1;
                        origin_x = clamp_artboard_x(event.x);
                        origin_y = clamp_artboard_y(event.y);
                        previous_x = origin_x;
                        previous_y = origin_y;
                        (void)phipia_pointer_capture(response.window, 1);
                        if (selected_tool == TOOL_BRUSH ||
                                selected_tool == TOOL_ERASER) {
                            const struct phipia_rect damage = brush_damage(
                                origin_x, origin_y, origin_x, origin_y, radius);
                            uint64_t present_started;

                            fill_circle(&canvas, origin_x, origin_y, radius,
                                selected_tool == TOOL_ERASER ? COLOR_PAPER :
                                    palette_colors[selected_color]);
                            ++stroke_segments;
                            present_started = phipia_monotonic_ns();
                            if (present(response.window, damage.x, damage.y,
                                    damage.width, damage.height) != 0) {
                                return 26;
                            }
                            present_elapsed += phipia_monotonic_ns() -
                                present_started;
                            if (damage.width * damage.height >
                                    maximum_damage_pixels) {
                                maximum_damage_pixels =
                                    damage.width * damage.height;
                            }
                            ++present_samples;
                            ++partial_presents;
                        }
                    }
                } else if (drawing != 0) {
                    if (selected_tool == TOOL_LINE ||
                            selected_tool == TOOL_RECTANGLE ||
                            selected_tool == TOOL_ELLIPSE) {
                        const int32_t x = clamp_artboard_x(event.x);
                        const int32_t y = clamp_artboard_y(event.y);
                        const struct phipia_rect damage = shape_damage(origin_x,
                            origin_y, x, y);

                        draw_shape(&canvas, selected_tool, origin_x, origin_y,
                            x, y, palette_colors[selected_color]);
                        ++stroke_segments;
                        if (present(response.window, damage.x, damage.y,
                                damage.width, damage.height) != 0) {
                            return 26;
                        }
                        ++partial_presents;
                    }
                    drawing = 0;
                    (void)phipia_pointer_capture(response.window, 0);
                }
            } else if (event.type == PHIPIA_EVENT_CLOSE) {
                running = 0;
            }
        }
    }
    printf("PHIPIA CANVAS PASS focus=%u key=%u pointer=%u strokes=%u colors=%u partial=%u\n",
        focus_events, key_events, pointer_events, stroke_segments,
        color_changes, partial_presents);
    if (present_samples != 0U) {
        printf("PHIPIA PERF canvas brush_damage_samples=%u max_pixels=%u total_ns=%llu average_ns=%llu\n",
            present_samples, maximum_damage_pixels,
            (unsigned long long)present_elapsed,
            (unsigned long long)(present_elapsed / present_samples));
    }
    if (phipia_handle_close(response.events) < 0 ||
        phipia_handle_close(response.window) < 0 || partial_presents == 0U ||
        focus_events == 0U || (proof_mode != 0 && key_events != 0U &&
            color_changes == 0U)) {
        return 27;
    }
    free(inter_font);
    free(tool_icons);
    return 0;
}
