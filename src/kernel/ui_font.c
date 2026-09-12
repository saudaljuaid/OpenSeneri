/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/ui_font.h>

#define INTER_WIDTH 16U
#define INTER_HEIGHT 19U
#define INTER_ASCENT 15U
#define INTER_DESCENT 4U
#define INTER_MAX_ADVANCE 15U
#define INTER_ROW_BYTES 16U
#define INTER_FIRST 0x20U
#define INTER_COUNT 95U
#define INTER_DATA_LENGTH 28975U
#define INTER_ASSET_LENGTH 28999U
#define INTER_FINGERPRINT UINT64_C(0xD4CC40D8355E676C)
#define LABEL_PIXEL_HASH UINT64_C(0x6CDD947CFD5228B2)

static bool verified;
static struct ui_font_metrics installed_metrics;
static const char *self_test_failure = "Phipia UI font self-test not run";

static bool add_u32(uint32_t left, uint32_t right, uint32_t *sum)
{
    if (sum == NULL || left > UINT32_MAX - right) {
        return false;
    }
    *sum = left + right;
    return true;
}

static bool metrics_are_inter(const struct ui_font_metrics *metrics)
{
    return metrics != NULL &&
        metrics->width == INTER_WIDTH &&
        metrics->height == INTER_HEIGHT &&
        metrics->ascent == INTER_ASCENT &&
        metrics->descent == INTER_DESCENT &&
        metrics->advance == INTER_MAX_ADVANCE &&
        metrics->row_bytes == INTER_ROW_BYTES &&
        metrics->first == INTER_FIRST &&
        metrics->count == INTER_COUNT &&
        metrics->data_length == INTER_DATA_LENGTH;
}

enum ui_font_status ui_font_initialize(void)
{
    struct ui_font_metrics metrics;
    const int32_t status = phipia_ui_font_geometry(&metrics);

    if (status != UI_FONT_STATUS_OK) {
        return (enum ui_font_status)status;
    }
    if (!metrics_are_inter(&metrics) ||
        phipia_ui_font_size() != INTER_ASSET_LENGTH ||
        phipia_ui_font_fingerprint() != INTER_FINGERPRINT) {
        return UI_FONT_STATUS_BAD_METRICS;
    }

    installed_metrics = metrics;
    verified = true;
    return UI_FONT_STATUS_OK;
}

bool ui_font_is_verified(void)
{
    return verified;
}

struct ui_font_metrics ui_font_get_metrics(void)
{
    return installed_metrics;
}

enum ui_font_status ui_font_text_width(const char *text, uint32_t *width)
{
    uint32_t length = 0U;

    if (text == NULL || width == NULL) {
        return UI_FONT_STATUS_NULL_ARGUMENT;
    }
    if (!verified) {
        return UI_FONT_STATUS_NOT_VERIFIED;
    }

    for (size_t index = 0U; text[index] != '\0'; ++index) {
        uint32_t advance;
        const int32_t status = phipia_ui_font_glyph_advance(
            (uint32_t)(unsigned char)text[index], &advance);

        if (status != UI_FONT_STATUS_OK) {
            return (enum ui_font_status)status;
        }
        if (length > UINT32_MAX - advance) {
            return UI_FONT_STATUS_SIZE_OVERFLOW;
        }
        length += advance;
    }
    *width = length;
    return UI_FONT_STATUS_OK;
}

static uint32_t blend_alpha(uint32_t under, uint32_t over, uint8_t alpha)
{
    const uint32_t inverse = UINT8_MAX - alpha;
    uint32_t result = under & UINT32_C(0xFF000000);

    for (uint32_t shift = 0U; shift <= 16U; shift += 8U) {
        const uint32_t lower = (under >> shift) & UINT32_C(0xFF);
        const uint32_t upper = (over >> shift) & UINT32_C(0xFF);
        const uint32_t channel =
            (lower * inverse + upper * alpha + 127U) / UINT8_MAX;

        result |= channel << shift;
    }
    return result;
}

static enum ui_font_status draw_with_metrics(
    const struct ui_font_metrics *metrics,
    struct surface *surface,
    struct surface_rect bounds,
    struct surface_rect clip,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    uint32_t foreground,
    uint32_t style,
    size_t *glyphs_drawn
)
{
    uint8_t bitmap[UI_FONT_MAX_HEIGHT * UI_FONT_MAX_ROW_BYTES];
    uint32_t bounds_right;
    uint32_t bounds_bottom;
    uint32_t clip_right;
    uint32_t clip_bottom;
    uint32_t glyph_top;
    uint32_t glyph_bottom;
    uint32_t pen = x;
    size_t drawn = 0U;

    if (metrics == NULL || surface == NULL || text == NULL) {
        return UI_FONT_STATUS_NULL_ARGUMENT;
    }
    if (!surface->active || bounds.width == 0U || bounds.height == 0U ||
        !add_u32(bounds.x, bounds.width, &bounds_right) ||
        !add_u32(bounds.y, bounds.height, &bounds_bottom) ||
        bounds_right > surface->width || bounds_bottom > surface->height ||
        clip.width == 0U || clip.height == 0U ||
        !add_u32(clip.x, clip.width, &clip_right) ||
        !add_u32(clip.y, clip.height, &clip_bottom) ||
        clip.x < bounds.x || clip.y < bounds.y ||
        clip_right > bounds_right || clip_bottom > bounds_bottom ||
        x < bounds.x || baseline < metrics->ascent) {
        return UI_FONT_STATUS_DESTINATION_CLIPPING_FAILURE;
    }
    glyph_top = baseline - metrics->ascent;
    if (!add_u32(baseline, metrics->descent, &glyph_bottom) ||
        glyph_top < bounds.y || glyph_bottom > bounds_bottom) {
        return UI_FONT_STATUS_DESTINATION_CLIPPING_FAILURE;
    }

    for (size_t index = 0U; text[index] != '\0'; ++index) {
        const uint32_t code = (uint32_t)(unsigned char)text[index];
        const int32_t status = phipia_ui_font_glyph(code, bitmap,
            sizeof(bitmap));
        uint32_t advance;
        int32_t advance_status;

        if (status != UI_FONT_STATUS_OK) {
            return (enum ui_font_status)status;
        }
        advance_status = phipia_ui_font_glyph_advance(code, &advance);
        if (advance_status != UI_FONT_STATUS_OK) {
            return (enum ui_font_status)advance_status;
        }

        for (uint32_t row = 0U; row < metrics->height; ++row) {
            for (uint32_t column = 0U; column < metrics->width; ++column) {
                const uint8_t alpha =
                    bitmap[row * metrics->row_bytes + column];
                const uint32_t destination_y = glyph_top + row;
                const uint32_t slant = (style & UI_FONT_STYLE_ITALIC) != 0U ?
                    (metrics->height - 1U - row) / 5U : 0U;
                const uint32_t weights =
                    (style & UI_FONT_STYLE_BOLD) != 0U ? 2U : 1U;

                if (alpha == 0U || destination_y < clip.y ||
                        destination_y >= clip_bottom) {
                    continue;
                }
                for (uint32_t weight = 0U; weight < weights; ++weight) {
                    uint32_t destination_x;

                    if (!add_u32(pen, column, &destination_x) ||
                            !add_u32(destination_x, slant + weight,
                                &destination_x) ||
                            destination_x < bounds.x ||
                            destination_x >= bounds_right ||
                            destination_x < clip.x ||
                            destination_x >= clip_right) {
                        continue;
                    }
                    uint32_t pixel = foreground;
                    if (alpha != UINT8_MAX) {
                        uint32_t under;

                        if (surface_read_pixel(surface, destination_x,
                                destination_y, &under) != SURFACE_STATUS_OK) {
                            return UI_FONT_STATUS_DESTINATION_CLIPPING_FAILURE;
                        }
                        pixel = blend_alpha(under, foreground, alpha);
                    }
                    if (surface_pixel(surface, destination_x, destination_y,
                            pixel) != SURFACE_STATUS_OK) {
                        return UI_FONT_STATUS_DESTINATION_CLIPPING_FAILURE;
                    }
                }
            }
        }

        drawn += 1U;
        if (!add_u32(pen, advance, &pen)) {
            return UI_FONT_STATUS_SIZE_OVERFLOW;
        }
    }

    if (glyphs_drawn != NULL) {
        *glyphs_drawn = drawn;
    }
    return UI_FONT_STATUS_OK;
}

enum ui_font_status ui_font_draw_text(
    struct surface *surface,
    struct surface_rect bounds,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    uint32_t foreground,
    size_t *glyphs_drawn
)
{
    if (!verified) {
        return UI_FONT_STATUS_NOT_VERIFIED;
    }
    return draw_with_metrics(&installed_metrics, surface, bounds, bounds, x,
        baseline, text, foreground, UI_FONT_STYLE_REGULAR, glyphs_drawn);
}

enum ui_font_status ui_font_draw_text_clipped(
    struct surface *surface,
    struct surface_rect bounds,
    struct surface_rect clip,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    uint32_t foreground,
    size_t *glyphs_drawn
)
{
    if (!verified) {
        return UI_FONT_STATUS_NOT_VERIFIED;
    }
    return draw_with_metrics(&installed_metrics, surface, bounds, clip, x,
        baseline, text, foreground, UI_FONT_STYLE_REGULAR, glyphs_drawn);
}

enum ui_font_status ui_font_draw_text_styled_clipped(
    struct surface *surface,
    struct surface_rect bounds,
    struct surface_rect clip,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    uint32_t foreground,
    uint32_t style,
    size_t *glyphs_drawn
)
{
    if (!verified) {
        return UI_FONT_STATUS_NOT_VERIFIED;
    }
    if ((style & ~(uint32_t)(UI_FONT_STYLE_BOLD | UI_FONT_STYLE_ITALIC)) !=
            0U) {
        return UI_FONT_STATUS_BAD_METRICS;
    }
    return draw_with_metrics(&installed_metrics, surface, bounds, clip, x,
        baseline, text, foreground, style, glyphs_drawn);
}

static uint64_t pixel_hash(const uint32_t *pixels, size_t count)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);

    for (size_t index = 0U; index < count; ++index) {
        hash ^= pixels[index];
        hash *= UINT64_C(0x100000001B3);
    }
    return hash;
}

bool ui_font_self_test(void)
{
    struct ui_font_metrics metrics;
    int32_t status;
    uint32_t pixels[96U * 19U] = { 0U };
    struct surface surface = {
        .active = true,
        .width = 96U,
        .height = 19U,
        .pitch = 96U * SURFACE_BYTES_PER_PIXEL,
        .pixels = pixels
    };
    const struct surface_rect whole = { 0U, 0U, 96U, 19U };
    const struct surface_rect short_box = { 0U, 0U, 96U, 18U };

    self_test_failure = "Phipia UI font self-test passed";
    if (phipia_ui_font_self_test() != 1) {
        self_test_failure = "UI font bounded parser refusals are incomplete";
        return false;
    }
    status = phipia_ui_font_geometry(&metrics);
    if (status != UI_FONT_STATUS_OK) {
        self_test_failure = ui_font_status_string((enum ui_font_status)status);
        return false;
    }
    if (!metrics_are_inter(&metrics) ||
        phipia_ui_font_size() != INTER_ASSET_LENGTH ||
        phipia_ui_font_fingerprint() != INTER_FINGERPRINT) {
        self_test_failure = "UI font pinned asset metrics or fingerprint changed";
        return false;
    }
    if (draw_with_metrics(&metrics, &surface, whole, whole, 0U, 15U, "PHIPIA",
            UINT32_C(0x00008E92), UI_FONT_STYLE_REGULAR, NULL) !=
                UI_FONT_STATUS_OK ||
        pixel_hash(pixels, sizeof(pixels) / sizeof(pixels[0])) !=
            LABEL_PIXEL_HASH) {
        self_test_failure = "UI font representative label pixels changed";
        return false;
    }
    if (draw_with_metrics(&metrics, &surface, short_box, short_box, 0U, 15U, "P",
            1U, UI_FONT_STYLE_REGULAR, NULL) !=
                UI_FONT_STATUS_DESTINATION_CLIPPING_FAILURE) {
        self_test_failure = "UI font destination clipping refusal failed";
        return false;
    }
    if (draw_with_metrics(&metrics, &surface, whole, whole, 0U, 15U, "\x01",
            1U, UI_FONT_STYLE_REGULAR, NULL) !=
                UI_FONT_STATUS_MISSING_GLYPH) {
        self_test_failure = "UI font missing-glyph refusal failed";
        return false;
    }
    return true;
}

const char *ui_font_self_test_failure(void)
{
    return self_test_failure;
}

const char *ui_font_status_string(enum ui_font_status status)
{
    static const char *const messages[] = {
        "ok",
        "null UI font argument",
        "UI font header is missing or malformed",
        "UI font version is unsupported",
        "UI font metrics are invalid",
        "UI font glyph is missing",
        "UI font bitmap is truncated",
        "UI font size arithmetic overflowed",
        "UI font glyph buffer is too small",
        "UI font destination clipping failed",
        "UI font has not been verified"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        (size_t)UI_FONT_STATUS_NOT_VERIFIED + 1U,
        "UI font status messages are out of sync");
    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown UI font status";
    }
    return messages[status];
}
