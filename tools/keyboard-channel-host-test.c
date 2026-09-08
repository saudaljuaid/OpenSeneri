/* SPDX-License-Identifier: GPL-3.0-only */
/* Mouse traffic must not become text or shortcut modifiers during app saves. */
#include <assert.h>
#include <stdio.h>
#include "../src/kernel/keyboard.c"

static const struct { uint8_t status, byte; } input[] = {
    { 1U, 0x1dU }, /* Control down. */
    { 1U, 0x1fU }, /* S down. */
    { 0x21U, 0x28U }, /* Mouse byte would translate to an apostrophe. */
    { 1U, 0x9fU }, /* S up. */
    { 1U, 0x9dU }  /* Control up. */
};
static size_t cursor;
static uint8_t next_status(void)
{ return cursor < sizeof(input) / sizeof(input[0]) ? input[cursor].status : 0U; }
static uint8_t next_data(void)
{
    assert(cursor < sizeof(input) / sizeof(input[0]));
    assert((input[cursor].status & KEYBOARD_STATUS_AUXILIARY_DATA) == 0U);
    return input[cursor++].byte;
}

int main(void)
{
    state.active = true;
    keyboard_drain_output(next_status, next_data);
    assert(cursor == 2U && queue_tail == 2U && state.control);
    assert(queue[1].character == 's' && queue[1].pressed && queue[1].control);
    keyboard_drain_output(next_status, next_data);
    assert(cursor == 2U && queue_tail == 2U && state.control);
    ++cursor; /* The pointer IRQ consumes its own byte. */
    keyboard_drain_output(next_status, next_data);
    assert(cursor == 5U && queue_tail == 4U && !state.control);
    assert(!queue[2].pressed && queue[2].control && queue[2].character == '\0');
    assert(!queue[3].pressed && !queue[3].control && queue[3].character == '\0');
    for (size_t index = 0U; index < queue_tail; ++index) assert(queue[index].character != '\'');
    puts("PS/2 mixed pointer and save shortcut input preserves channel ownership and exact characters: PASS");
    return 0;
}
