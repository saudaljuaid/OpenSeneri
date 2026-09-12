/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/interrupts.h>
#include <phipia/ioapic.h>
#include <phipia/keyboard.h>
#include <phipia/pointer.h>
#include <phipia/ui.h>

#define PS2_DATA_PORT UINT16_C(0x0060)
#define PS2_COMMAND_PORT UINT16_C(0x0064)
#define PS2_STATUS_OUTPUT_FULL UINT8_C(0x01)
#define PS2_STATUS_INPUT_FULL UINT8_C(0x02)
#define PS2_STATUS_AUXILIARY_DATA UINT8_C(0x20)
#define PS2_WAIT_LIMIT UINT32_C(100000)

#define PS2_COMMAND_READ_CONFIGURATION UINT8_C(0x20)
#define PS2_COMMAND_WRITE_CONFIGURATION UINT8_C(0x60)
#define PS2_COMMAND_DISABLE_SECOND_PORT UINT8_C(0xA7)
#define PS2_COMMAND_ENABLE_SECOND_PORT UINT8_C(0xA8)
#define PS2_COMMAND_TEST_SECOND_PORT UINT8_C(0xA9)
#define PS2_COMMAND_WRITE_AUXILIARY_OUTPUT UINT8_C(0xD3)
#define PS2_COMMAND_WRITE_SECOND_PORT UINT8_C(0xD4)

#define PS2_CONFIGURATION_SECOND_INTERRUPT UINT8_C(0x02)
#define PS2_CONFIGURATION_SECOND_CLOCK_OFF UINT8_C(0x20)

#define POINTER_DEVICE_SET_DEFAULTS UINT8_C(0xF6)
#define POINTER_DEVICE_ENABLE_REPORTING UINT8_C(0xF4)
#define POINTER_DEVICE_ACK UINT8_C(0xFA)

#define POINTER_PACKET_ALWAYS_ONE UINT8_C(0x08)
#define POINTER_PACKET_LEFT UINT8_C(0x01)
#define POINTER_PACKET_RIGHT UINT8_C(0x02)
#define POINTER_PACKET_MIDDLE UINT8_C(0x04)
#define POINTER_PACKET_X_SIGN UINT8_C(0x10)
#define POINTER_PACKET_Y_SIGN UINT8_C(0x20)
#define POINTER_PACKET_X_OVERFLOW UINT8_C(0x40)
#define POINTER_PACKET_Y_OVERFLOW UINT8_C(0x80)

#define POINTER_IRQ UINT8_C(12)
#define POINTER_VECTOR ((uint8_t)(INTERRUPT_IOAPIC_BASE + POINTER_IRQ))

static struct pointer_state state;
static const char *self_test_failure = "PS/2 pointer self-test not run";

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static bool wait_to_write(void)
{
    for (uint32_t spins = 0U; spins < PS2_WAIT_LIMIT; ++spins) {
        if ((inb(PS2_COMMAND_PORT) & PS2_STATUS_INPUT_FULL) == 0U) {
            return true;
        }
    }
    return false;
}

static bool wait_to_read(uint8_t *status)
{
    for (uint32_t spins = 0U; spins < PS2_WAIT_LIMIT; ++spins) {
        const uint8_t current = inb(PS2_COMMAND_PORT);

        if ((current & PS2_STATUS_OUTPUT_FULL) != 0U) {
            if (status != NULL) {
                *status = current;
            }
            return true;
        }
    }
    return false;
}

static enum pointer_status send_command(uint8_t command)
{
    if (!wait_to_write()) {
        return POINTER_STATUS_CONTROLLER_TIMEOUT;
    }
    outb(PS2_COMMAND_PORT, command);
    return POINTER_STATUS_OK;
}

static enum pointer_status send_data(uint8_t value)
{
    if (!wait_to_write()) {
        return POINTER_STATUS_CONTROLLER_TIMEOUT;
    }
    outb(PS2_DATA_PORT, value);
    return POINTER_STATUS_OK;
}

static enum pointer_status receive_data(uint8_t *value, bool auxiliary)
{
    uint8_t status;

    if (value == NULL) {
        return POINTER_STATUS_DEVICE_REFUSED;
    }
    if (!wait_to_read(&status)) {
        return POINTER_STATUS_CONTROLLER_TIMEOUT;
    }
    if (((status & PS2_STATUS_AUXILIARY_DATA) != 0U) != auxiliary) {
        return POINTER_STATUS_DEVICE_REFUSED;
    }
    *value = inb(PS2_DATA_PORT);
    return POINTER_STATUS_OK;
}

static enum pointer_status write_configuration(uint8_t configuration)
{
    enum pointer_status status =
        send_command(PS2_COMMAND_WRITE_CONFIGURATION);

    return status == POINTER_STATUS_OK ? send_data(configuration) : status;
}

static enum pointer_status send_device_command(uint8_t command)
{
    uint8_t answer;
    enum pointer_status status = send_command(PS2_COMMAND_WRITE_SECOND_PORT);

    if (status == POINTER_STATUS_OK) {
        status = send_data(command);
    }
    if (status == POINTER_STATUS_OK) {
        status = receive_data(&answer, true);
    }
    if (status == POINTER_STATUS_OK && answer != POINTER_DEVICE_ACK) {
        status = POINTER_STATUS_DEVICE_REFUSED;
    }
    return status;
}

static uint32_t clamp_coordinate(int64_t value, uint32_t bound)
{
    if (value <= 0 || bound == 0U) {
        return 0U;
    }
    if ((uint64_t)value >= (uint64_t)bound) {
        return bound - 1U;
    }
    return (uint32_t)value;
}

static void publish_movement(struct pointer_state *target, bool publish)
{
    const struct ui_event event = {
        .type = UI_EVENT_POINTER_MOVEMENT,
        .point = { (int32_t)target->x, (int32_t)target->y },
        .button = UI_POINTER_BUTTON_NONE
    };

    target->movements += 1U;
    if (publish) {
        (void)ui_event_publish(&event);
    }
}

static void publish_button(
    struct pointer_state *target,
    bool publish,
    enum ui_pointer_button button,
    bool pressed
)
{
    const struct ui_event event = {
        .type = pressed ? UI_EVENT_POINTER_BUTTON_PRESS :
            UI_EVENT_POINTER_BUTTON_RELEASE,
        .point = { (int32_t)target->x, (int32_t)target->y },
        .button = button
    };

    target->button_transitions += 1U;
    if (publish) {
        (void)ui_event_publish(&event);
    }
}

static void decode_packet(struct pointer_state *target, bool publish)
{
    const uint8_t flags = target->packet[0];
    const bool left = (flags & POINTER_PACKET_LEFT) != 0U;
    const bool right = (flags & POINTER_PACKET_RIGHT) != 0U;
    const bool middle = (flags & POINTER_PACKET_MIDDLE) != 0U;

    target->packets += 1U;
    if ((flags & (POINTER_PACKET_X_OVERFLOW | POINTER_PACKET_Y_OVERFLOW)) != 0U) {
        target->overflows += 1U;
        return;
    }

    const int16_t delta_x = (int16_t)(int8_t)target->packet[1];
    const int16_t delta_y = -(int16_t)(int8_t)target->packet[2];
    const uint32_t old_x = target->x;
    const uint32_t old_y = target->y;

    target->x = clamp_coordinate((int64_t)target->x + delta_x,
        target->bound_width);
    target->y = clamp_coordinate((int64_t)target->y + delta_y,
        target->bound_height);
    if (target->x != old_x || target->y != old_y) {
        publish_movement(target, publish);
    }

    if (left != target->left) {
        publish_button(target, publish, UI_POINTER_BUTTON_LEFT, left);
    }
    if (middle != target->middle) {
        publish_button(target, publish, UI_POINTER_BUTTON_MIDDLE, middle);
    }
    if (right != target->right) {
        publish_button(target, publish, UI_POINTER_BUTTON_RIGHT, right);
    }
    target->left = left;
    target->middle = middle;
    target->right = right;
}

static void decode_byte(struct pointer_state *target, uint8_t byte, bool publish)
{
    target->bytes += 1U;
    if (target->packet_index == 0U &&
        (byte & POINTER_PACKET_ALWAYS_ONE) == 0U) {
        target->desynchronizations += 1U;
        return;
    }

    target->packet[target->packet_index] = byte;
    target->packet_index += 1U;
    if (target->packet_index == POINTER_PACKET_SIZE) {
        target->packet_index = 0U;
        decode_packet(target, publish);
    }
}

static void pointer_interrupt(struct interrupt_frame *frame, void *context)
{
    (void)frame;
    (void)context;
    state.interrupts += 1U;

    for (uint32_t bytes = 0U; bytes < UI_EVENT_QUEUE_CAPACITY; ++bytes) {
        const uint8_t status = inb(PS2_COMMAND_PORT);

        if ((status & PS2_STATUS_OUTPUT_FULL) == 0U ||
            (status & PS2_STATUS_AUXILIARY_DATA) == 0U) {
            return;
        }
        decode_byte(&state, inb(PS2_DATA_PORT), true);
    }
}

static enum pointer_status decide_absent(enum pointer_status reason)
{
    (void)send_command(PS2_COMMAND_DISABLE_SECOND_PORT);
    state.decided = true;
    state.present = false;
    state.active = false;
    return reason;
}

enum pointer_status pointer_initialize(void)
{
    uint8_t configuration;
    uint8_t answer;
    enum pointer_status status;

    if (state.decided) {
        return POINTER_STATUS_ALREADY_DECIDED;
    }
    if (!keyboard_is_initialized()) {
        return POINTER_STATUS_KEYBOARD_REQUIRED;
    }
    if (cpu_interrupts_enabled()) {
        return POINTER_STATUS_INTERRUPTS_ENABLED;
    }
    if (!ioapic_is_initialized()) {
        return POINTER_STATUS_NO_IOAPIC;
    }

    status = send_command(PS2_COMMAND_ENABLE_SECOND_PORT);
    if (status != POINTER_STATUS_OK) {
        return decide_absent(status);
    }
    status = send_command(PS2_COMMAND_READ_CONFIGURATION);
    if (status == POINTER_STATUS_OK) {
        status = receive_data(&configuration, false);
    }
    if (status != POINTER_STATUS_OK) {
        return decide_absent(status);
    }
    if ((configuration & PS2_CONFIGURATION_SECOND_CLOCK_OFF) != 0U) {
        return decide_absent(POINTER_STATUS_AUXILIARY_CLOCK_STUCK);
    }

    status = send_command(PS2_COMMAND_TEST_SECOND_PORT);
    if (status == POINTER_STATUS_OK) {
        status = receive_data(&answer, false);
    }
    if (status != POINTER_STATUS_OK || answer != 0U) {
        return decide_absent(POINTER_STATUS_PORT_TEST_FAILED);
    }

    if (interrupt_register_handler(POINTER_VECTOR, pointer_interrupt, NULL) !=
        INTERRUPT_STATUS_OK) {
        return decide_absent(POINTER_STATUS_HANDLER_FAILURE);
    }
    if (ioapic_route_isa_irq(POINTER_IRQ, POINTER_VECTOR, 0U) !=
        IOAPIC_STATUS_OK) {
        (void)interrupt_unregister_handler(POINTER_VECTOR);
        return decide_absent(POINTER_STATUS_IOAPIC_FAILURE);
    }

    status = send_device_command(POINTER_DEVICE_SET_DEFAULTS);
    if (status == POINTER_STATUS_OK) {
        status = send_device_command(POINTER_DEVICE_ENABLE_REPORTING);
    }
    if (status != POINTER_STATUS_OK) {
        (void)ioapic_mask_isa_irq(POINTER_IRQ);
        (void)interrupt_unregister_handler(POINTER_VECTOR);
        return decide_absent(status);
    }

    configuration &= (uint8_t)~PS2_CONFIGURATION_SECOND_CLOCK_OFF;
    configuration |= PS2_CONFIGURATION_SECOND_INTERRUPT;
    status = write_configuration(configuration);
    if (status != POINTER_STATUS_OK) {
        (void)ioapic_mask_isa_irq(POINTER_IRQ);
        (void)interrupt_unregister_handler(POINTER_VECTOR);
        return decide_absent(status);
    }

    state.decided = true;
    state.present = true;
    state.active = true;
    state.bound_width = 1U;
    state.bound_height = 1U;
    return POINTER_STATUS_OK;
}

enum pointer_status pointer_set_bounds(uint32_t width, uint32_t height)
{
    if (!state.present || !state.active) {
        return POINTER_STATUS_NOT_AVAILABLE;
    }
    if (width == 0U || height == 0U || width > INT32_MAX ||
        height > INT32_MAX) {
        return POINTER_STATUS_BAD_BOUNDS;
    }
    state.bound_width = width;
    state.bound_height = height;
    state.x = width - width / 4U;
    state.y = height / 3U;
    return POINTER_STATUS_OK;
}

struct pointer_state pointer_get_state(void)
{
    return state;
}

bool pointer_is_present(void)
{
    return state.present && state.active;
}

enum pointer_status pointer_inject_packet(
    uint8_t flags,
    uint8_t delta_x,
    uint8_t delta_y
)
{
    const uint8_t packet[POINTER_PACKET_SIZE] = {
        (uint8_t)(flags | POINTER_PACKET_ALWAYS_ONE), delta_x, delta_y
    };

    if (!pointer_is_present()) {
        return POINTER_STATUS_NOT_AVAILABLE;
    }
    if (!cpu_interrupts_enabled()) {
        return POINTER_STATUS_INJECTION_FAILURE;
    }

    for (size_t index = 0U; index < POINTER_PACKET_SIZE; ++index) {
        const uint64_t before = state.bytes;
        enum pointer_status status =
            send_command(PS2_COMMAND_WRITE_AUXILIARY_OUTPUT);

        if (status == POINTER_STATUS_OK) {
            status = send_data(packet[index]);
        }
        if (status != POINTER_STATUS_OK) {
            return status;
        }
        for (uint64_t spins = 0U; spins < UINT64_C(20000000); ++spins) {
            if (state.bytes != before) {
                break;
            }
        }
        if (state.bytes == before) {
            return POINTER_STATUS_INJECTION_FAILURE;
        }
    }
    return POINTER_STATUS_OK;
}

static void feed_packet(
    struct pointer_state *test,
    uint8_t flags,
    uint8_t delta_x,
    uint8_t delta_y
)
{
    decode_byte(test, (uint8_t)(flags | POINTER_PACKET_ALWAYS_ONE), false);
    decode_byte(test, delta_x, false);
    decode_byte(test, delta_y, false);
}

bool pointer_self_test(void)
{
    struct pointer_state test = {
        .active = true,
        .present = true,
        .x = 50U,
        .y = 50U,
        .bound_width = 100U,
        .bound_height = 100U
    };

    self_test_failure = "PS/2 pointer self-test passed";
    feed_packet(&test, 0U, 5U, 3U);
    if (test.x != 55U || test.y != 47U || test.movements != 1U) {
        self_test_failure = "PS/2 positive packet delta is incorrect";
        return false;
    }
    feed_packet(&test, POINTER_PACKET_X_SIGN | POINTER_PACKET_Y_SIGN,
        UINT8_C(0xFC), UINT8_C(0xFE));
    if (test.x != 51U || test.y != 49U) {
        self_test_failure = "PS/2 signed negative packet delta is incorrect";
        return false;
    }

    const uint32_t before_x = test.x;
    const uint32_t before_y = test.y;
    feed_packet(&test, POINTER_PACKET_X_OVERFLOW |
        POINTER_PACKET_Y_OVERFLOW, UINT8_C(0x7F), UINT8_C(0x7F));
    if (test.x != before_x || test.y != before_y || test.overflows != 1U) {
        self_test_failure = "PS/2 overflow packet changed pointer state";
        return false;
    }

    decode_byte(&test, 0U, false);
    if (test.desynchronizations != 1U || test.packet_index != 0U) {
        self_test_failure = "PS/2 packet desynchronization did not recover";
        return false;
    }
    feed_packet(&test, POINTER_PACKET_LEFT, 0U, 0U);
    feed_packet(&test, 0U, 0U, 0U);
    if (test.left || test.button_transitions != 2U) {
        self_test_failure = "PS/2 resynchronization produced a phantom button state";
        return false;
    }

    test.x = 0U;
    test.y = 0U;
    feed_packet(&test, POINTER_PACKET_X_SIGN, UINT8_C(0x80), 0U);
    feed_packet(&test, 0U, 0U, UINT8_C(0x7F));
    if (test.x != 0U || test.y != 0U) {
        self_test_failure = "PS/2 pointer did not clamp at the top-left corner";
        return false;
    }
    test.x = test.bound_width - 1U;
    test.y = test.bound_height - 1U;
    feed_packet(&test, 0U, UINT8_C(0x7F), UINT8_C(0x80));
    if (test.x != test.bound_width - 1U ||
        test.y != test.bound_height - 1U) {
        self_test_failure = "PS/2 pointer did not clamp at the bottom-right corner";
        return false;
    }
    return true;
}

const char *pointer_self_test_failure(void)
{
    return self_test_failure;
}

const char *pointer_status_string(enum pointer_status status)
{
    static const char *const messages[] = {
        "ok",
        "PS/2 auxiliary device is absent",
        "pointer availability was already decided",
        "pointer discovery requires the shared keyboard controller",
        "pointer discovery ran with interrupts enabled",
        "pointer discovery requires the I/O APIC",
        "the shared 8042 controller timed out",
        "the PS/2 auxiliary clock stayed disabled",
        "the PS/2 auxiliary port failed its test",
        "the PS/2 auxiliary device refused a command",
        "the pointer interrupt handler could not be installed",
        "IRQ12 could not be routed through the I/O APIC",
        "pointer input is not available",
        "pointer bounds are invalid",
        "the pointer test packet was not delivered"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        (size_t)POINTER_STATUS_INJECTION_FAILURE + 1U,
        "pointer status messages are out of sync");
    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown pointer status";
    }
    return messages[status];
}
