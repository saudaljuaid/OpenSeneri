/* SPDX-License-Identifier: GPL-3.0-only */
/* Application journeys must survive animation origins below a short taskbar. */
#include <assert.h>
#include <stdio.h>
#define ui_anim_draw checked_animation_draw
#include "../src/kernel/ui.c"
#undef ui_anim_draw

static unsigned draws;
void console_serial_write(const char *message) { (void)message; }
void console_serial_write_u64(uint64_t value) { (void)value; }

enum ui_anim_status checked_animation_draw(struct ui_anim *anim,
    struct surface *target, struct ui_rect clip)
{
    (void)anim;
    if (clip.x > target->width || clip.width > target->width - clip.x ||
        clip.y > target->height || clip.height > target->height - clip.y)
        return UI_ANIM_STATUS_BAD_GEOMETRY;
    ++draws;
    assert(clip.x == 164U && clip.y == 106U && clip.width == 860U && clip.height == 662U);
    return UI_ANIM_STATUS_OK;
}

int main(void)
{
    struct surface destination = { .active = true, .width = 1024U, .height = 768U };
    canvas = &destination;
    state.layout.surface = (struct ui_rect){ 0U, 0U, 1024U, 768U };
    panel_anim.frame = (struct ui_rect){ 164U, 106U, 860U, 600U };
    panel_anim.origin = (struct ui_rect){ 483U, 728U, 58U, 58U };
    const struct ui_rect invalidation = ui_anim_bounds(&panel_anim);
    assert(invalidation.y + invalidation.height == 786U);
    assert(draw_animated_panel(invalidation) == UI_STATUS_OK && draws == 1U);
    assert(draw_animated_panel((struct ui_rect){ 0U, 768U, 1024U, 18U }) == UI_STATUS_OK && draws == 1U);
    puts("Application animation offscreen origin: visible redraw and empty clipping PASS");
    return 0;
}
