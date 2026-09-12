/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Editor: a video editor with two libraries in it.
 *
 * The ARRANGEMENT is CapCut's, which is the arrangement every desktop
 * editor since Premiere has settled on and which CapCut is the plainest
 * statement of: a library down the left with a tab strip at its head, the
 * player in the middle with its transport under it, a properties panel down
 * the right showing whatever is selected, and the timeline across the
 * bottom carrying one lane per kind of thing.  The chrome is Windows 10's -
 * a 32-pixel caption, square corners, 46-pixel caption buttons, one pixel
 * of accent round the window - over the dark editing surface an editor
 * wants, which is what Windows 10's own Video Editor did.
 *
 * WHAT IT LEAVES OUT is most of CapCut.  Two libraries: text, and effects.
 * No media importer, no transitions, no stickers, no audio, no
 * auto-captions, no background removal, nothing with a model behind it.
 * Everything drawn in this window does the thing it is drawn as, and a
 * library of buttons that open onto an apology is the one thing this shell
 * does not ship.
 *
 * THE CLIP IS THE CALLER'S.  This module has no decoder and no filesystem.
 * The picture the player shows arrives through editor_set_poster() as
 * finished pixels, at the size the stage draws it, and is composited one to
 * one; the clip's name and duration arrive beside it.  What the module owns
 * is what it can actually do to that picture: the four effects it knows how
 * to apply, and the three text styles it knows how to set.
 */

#ifndef PHIPIA_EDITOR_H
#define PHIPIA_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/surface.h>
#include <phipia/ui.h>

enum editor_status {
    EDITOR_STATUS_OK = 0,
    EDITOR_STATUS_NULL_ARGUMENT,
    EDITOR_STATUS_NOT_INITIALIZED,
    EDITOR_STATUS_BAD_INDEX,
    EDITOR_STATUS_FULL,
    EDITOR_STATUS_UNSUPPORTED_GEOMETRY,
    EDITOR_STATUS_SURFACE_FAILURE
};

/*
 * The lanes.  One for the clip the caller supplied, one for text and one
 * for effects - which is the whole of what this editor edits.
 */
enum editor_track {
    EDITOR_TRACK_CLIP = 0,
    EDITOR_TRACK_TEXT,
    EDITOR_TRACK_EFFECT,
    EDITOR_TRACK_COUNT
};

/*
 * The effects, which are the four this module can actually apply to the
 * pixels it is handed.  Nothing here needs a model, a network or a second
 * frame: each is a function of one pixel and where it sits.
 */
enum editor_effect {
    EDITOR_EFFECT_FADE = 0,   /* to black, over the item's own length */
    EDITOR_EFFECT_MONO,       /* the luminance, at full weight        */
    EDITOR_EFFECT_WARM,       /* pulled towards the warm end          */
    EDITOR_EFFECT_VIGNETTE,   /* darkened away from the centre        */
    EDITOR_EFFECT_COUNT
};

/* The text styles, which are the three sizes and weights it can set. */
enum editor_style {
    EDITOR_STYLE_CAPTION = 0, /* small, low, the way a subtitle sits  */
    EDITOR_STYLE_BODY,        /* the middle of the frame              */
    EDITOR_STYLE_TITLE,       /* the heading font, high in the frame  */
    EDITOR_STYLE_COUNT
};

#define EDITOR_MAX_ITEMS 12U
#define EDITOR_TEXT_BYTES 40U

struct editor_item {
    bool present;
    enum editor_track track;
    uint32_t start_ms;
    uint32_t length_ms;
    /* The words, for a text item.  An effect item ignores it and takes its
     * name from the effect. */
    char label[EDITOR_TEXT_BYTES];
    enum editor_style style;
    enum editor_effect effect;
    /* 0..100.  How far a text item is faded up, or how hard an effect is
     * laid on.  An item at 0 draws nothing, which is what makes the slider
     * in the properties panel worth having. */
    uint8_t strength;
};

/* The clip under everything, which is the caller's. */
struct editor_clip {
    char name[EDITOR_TEXT_BYTES];
    uint32_t length_ms;
};

enum editor_status editor_initialize(struct surface *canvas,
    struct ui_rect frame);
enum editor_status editor_set_frame(struct ui_rect frame);
struct ui_rect editor_bounds(void);
/* Where the player draws its picture, so a caller can crop one to fit. */
struct ui_rect editor_stage_rect(void);

enum editor_status editor_set_clip(const struct editor_clip *clip);
/*
 * The picture the player shows.  `pixels` is 0x00RRGGBB, `width` by
 * `height`, and is drawn ONE TO ONE centred in the stage - nothing here
 * resamples, so a caller that wants it to fill the stage hands over a
 * picture the size editor_stage_rect() reports.  Passing NULL clears it and
 * the player draws its empty state.
 */
enum editor_status editor_set_poster(const uint32_t *pixels, uint32_t width,
    uint32_t height);

enum editor_status editor_set_item(size_t index,
    const struct editor_item *item);
const struct editor_item *editor_item(size_t index);
size_t editor_item_count(void);
/* Which item the properties panel is showing, or (size_t)-1 for none. */
size_t editor_selected(void);
enum editor_status editor_select(size_t index, struct ui_rect *damage);

/* The playhead, in milliseconds from the start of the clip. */
uint32_t editor_playhead_ms(void);
enum editor_status editor_seek(uint32_t position_ms, struct ui_rect *damage);
bool editor_playing(void);
enum editor_status editor_set_playing(bool playing, struct ui_rect *damage);

/* Open and close, and the taskbar button a compositor hangs off them. */
enum editor_status editor_open(struct ui_rect *damage);
enum editor_status editor_close(struct ui_rect *damage);
bool editor_is_open(void);

enum editor_status editor_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum editor_status editor_pointer_press(struct ui_point point,
    struct ui_rect *damage);
enum editor_status editor_key_escape(struct ui_rect *damage);

/* The playhead advancing while it plays, and the tab strip's cross-fade. */
bool editor_animate(struct ui_rect *damage);
bool editor_animating(void);

enum editor_status editor_draw(struct ui_rect damage);

bool editor_self_test(void);
const char *editor_self_test_failure(void);
const char *editor_status_string(enum editor_status status);

#endif /* PHIPIA_EDITOR_H */
