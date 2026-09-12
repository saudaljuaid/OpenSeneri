/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Paint.
 *
 * Windows 10's mspaint is the densest ribbon of the lot, and the order of its
 * seven Home groups is the whole of what makes it recognisable at a glance:
 *
 *   CLIPBOARD  Paste over Cut and Copy.
 *   IMAGE      Select, then Crop, Resize and Rotate stacked beside it.
 *   TOOLS      A 3x2 grid: pencil, fill, text, eraser, colour picker,
 *              magnifier.  Six buttons, never a row.
 *   BRUSHES    One tall button with a dropdown under it.
 *   SHAPES     A scrolling gallery three rows deep, then Outline and Fill
 *              stacked to its right.
 *   SIZE       One tall button showing four rules of increasing weight.
 *   COLORS     Color 1 and Color 2 side by side, then the twenty-swatch
 *              palette in two rows of ten, then Edit colors.
 *
 * The twenty swatches are not invented.  They are Paint's own default
 * palette, the same twenty values it has shipped with since Windows 7, and
 * they are listed with their names in PALETTE below.
 *
 * What Phipia does differently:
 *
 *   The selected tool and the selected shape take the ACCENT, not a grey
 *   pressed plate.  Paint marks them so faintly that on a busy ribbon you
 *   have to look twice to see which tool you are holding.
 *
 *   The two colour buttons SHOW THEIR HEX.  Paint shows a swatch and leaves
 *   you to open Edit colors to find out what it is, which is the one thing
 *   anyone opening Paint for a screenshot actually needs.
 *
 *   The zoom in the status bar is a real slider with the accent on the used
 *   half, rather than a grey track with a grey thumb.
 */
#ifndef PHIPIA_PAINT_H
#define PHIPIA_PAINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/cursor.h>
#include <phipia/ui.h>

/* Paint's six tools, in the order its 3x2 grid puts them. */
enum paint_tool {
    PAINT_TOOL_PENCIL = 0,
    PAINT_TOOL_FILL,
    PAINT_TOOL_TEXT,
    PAINT_TOOL_ERASER,
    PAINT_TOOL_PICKER,
    PAINT_TOOL_MAGNIFIER,
    PAINT_TOOL_COUNT
};

#define PAINT_SWATCHES 20U      /* two rows of ten */
#define PAINT_SHAPES 21U        /* three rows of seven */
#define PAINT_MAX_ROW_BYTES (1024U * 3U + 4U)

struct paint_image_info {
    uint32_t width;
    uint32_t height;
    uint32_t row_stride;
    bool dirty;
};

enum paint_status {
    PAINT_STATUS_OK = 0,
    PAINT_STATUS_NULL_ARGUMENT,
    PAINT_STATUS_NOT_INITIALIZED,
    PAINT_STATUS_BAD_INDEX,
    PAINT_STATUS_UNSUPPORTED_GEOMETRY,
    PAINT_STATUS_SURFACE_FAILURE,
    PAINT_STATUS_ALLOCATION_FAILURE
};

const char *paint_status_string(enum paint_status status);

enum paint_status paint_initialize(struct surface *canvas,
    struct ui_rect frame);
enum paint_status paint_set_frame(struct ui_rect frame);
struct ui_rect paint_bounds(void);
/* The white sheet inside the window, which is what a caller draws on. */
struct ui_rect paint_sheet_bounds(void);

/* A crosshair over the sheet, an arrow everywhere else.  The compositor
 * asks; this answers.  See phipia/cursor.h. */
enum cursor_kind paint_cursor_at(struct ui_point point);

enum paint_status paint_set_tool(enum paint_tool tool);
enum paint_status paint_set_shape(size_t shape);
/* Which of the twenty swatches Color 1 and Color 2 are showing. */
enum paint_status paint_set_colours(size_t first, size_t second);
enum paint_status paint_set_zoom(uint32_t percent);
enum paint_status paint_set_title(const char *title);
enum paint_status paint_set_focus(bool focused);
enum paint_status paint_resize_image(uint32_t width, uint32_t height);

enum paint_status paint_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum paint_status paint_pointer_press(struct ui_point point,
    struct ui_rect *damage);
enum paint_status paint_pointer_release(struct ui_point point,
    struct ui_rect *damage);

enum paint_status paint_text_input(char character, struct ui_rect *damage);
enum paint_status paint_key_backspace(struct ui_rect *damage);
enum paint_status paint_key_enter(struct ui_rect *damage);

/* The title-bar save button is rendered by Paint but fulfilled by the shell's
 * durable filesystem adapter.  Taking the request clears it. */
bool paint_take_save_request(void);
void paint_mark_saved(void);
struct paint_image_info paint_image(void);
enum paint_status paint_copy_bgr24_row(uint32_t row, uint8_t *destination,
    size_t capacity, size_t *written);

enum paint_status paint_draw(struct ui_rect damage);

bool paint_self_test(void);
const char *paint_self_test_failure(void);

#endif
