/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_UI_FONT_H
#define PHIPIA_UI_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>

#define UI_FONT_MAX_WIDTH 20U
#define UI_FONT_MAX_HEIGHT 32U
#define UI_FONT_MAX_ROW_BYTES UI_FONT_MAX_WIDTH

enum ui_font_status {
    UI_FONT_STATUS_OK = 0,
    UI_FONT_STATUS_NULL_ARGUMENT = 1,
    UI_FONT_STATUS_MALFORMED_HEADER = 2,
    UI_FONT_STATUS_UNSUPPORTED_VERSION = 3,
    UI_FONT_STATUS_BAD_METRICS = 4,
    UI_FONT_STATUS_MISSING_GLYPH = 5,
    UI_FONT_STATUS_TRUNCATED_BITMAP = 6,
    UI_FONT_STATUS_SIZE_OVERFLOW = 7,
    UI_FONT_STATUS_BUFFER_TOO_SMALL = 8,
    UI_FONT_STATUS_DESTINATION_CLIPPING_FAILURE = 9,
    UI_FONT_STATUS_NOT_VERIFIED = 10
};

enum ui_font_style {
    UI_FONT_STYLE_REGULAR = 0,
    UI_FONT_STYLE_BOLD = 1U << 0,
    UI_FONT_STYLE_ITALIC = 1U << 1
};

struct ui_font_metrics {
    uint32_t width;
    uint32_t height;
    uint32_t ascent;
    uint32_t descent;
    uint32_t advance;
    uint32_t row_bytes;
    uint32_t first;
    uint32_t count;
    uint32_t data_length;
};

int32_t phipia_ui_font_self_test(void);
size_t phipia_ui_font_size(void);
uint64_t phipia_ui_font_fingerprint(void);
int32_t phipia_ui_font_geometry(struct ui_font_metrics *metrics);
int32_t phipia_ui_font_glyph(
    uint32_t code,
    uint8_t *out,
    size_t out_len
);
int32_t phipia_ui_font_glyph_advance(uint32_t code, uint32_t *advance);

/* Validate the exact built-in asset and retain only its copied metrics. */
enum ui_font_status ui_font_initialize(void);
bool ui_font_is_verified(void);
struct ui_font_metrics ui_font_get_metrics(void);

enum ui_font_status ui_font_text_width(
    const char *text,
    uint32_t *width
);

/*
 * Draw transparent glyph foreground at x and baseline, clipped to bounds.
 * bounds is half-open and must itself lie inside the surface. Vertical glyph
 * extent must fit completely; horizontal overhang is clipped deterministically.
 */
enum ui_font_status ui_font_draw_text(
    struct surface *surface,
    struct surface_rect bounds,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    uint32_t foreground,
    size_t *glyphs_drawn
);
enum ui_font_status ui_font_draw_text_clipped(
    struct surface *surface,
    struct surface_rect bounds,
    struct surface_rect clip,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    uint32_t foreground,
    size_t *glyphs_drawn
);
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
);

bool ui_font_self_test(void);
const char *ui_font_self_test_failure(void);
const char *ui_font_status_string(enum ui_font_status status);

#endif
