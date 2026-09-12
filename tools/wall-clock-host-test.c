/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/wall_clock.h>

#define TEST_RTC_SECONDS UINT8_C(0x00)
#define TEST_RTC_STATUS_A UINT8_C(0x0A)
#define TEST_RTC_STATUS_B UINT8_C(0x0B)

enum rtc_test_mode {
    RTC_TEST_STABLE = 0,
    RTC_TEST_ROLLOVER,
    RTC_TEST_UNSTABLE,
    RTC_TEST_UIP_STUCK
};

static uint8_t cmos[256];
static uint8_t selected_register;
static enum rtc_test_mode mode;
static size_t seconds_reads;
static bool interrupts_enabled = true;

bool cpu_interrupts_enabled(void)
{
    return interrupts_enabled;
}

void cpu_interrupt_disable(void)
{
    interrupts_enabled = false;
}

void cpu_interrupt_enable(void)
{
    interrupts_enabled = true;
}

void cpu_out8(uint16_t port, uint8_t value)
{
    if (port == UINT16_C(0x70)) {
        selected_register = value;
    }
}

uint8_t cpu_in8(uint16_t port)
{
    if (port != UINT16_C(0x71)) {
        return 0U;
    }
    if (selected_register == TEST_RTC_STATUS_A) {
        return mode == RTC_TEST_UIP_STUCK ? UINT8_C(0x80) : 0U;
    }
    if (selected_register == TEST_RTC_SECONDS) {
        ++seconds_reads;
        if (mode == RTC_TEST_ROLLOVER && seconds_reads >= 2U) {
            return 0x59U;
        }
        if (mode == RTC_TEST_UNSTABLE) {
            return seconds_reads % 2U == 0U ? 0x59U : 0x58U;
        }
    }
    return cmos[selected_register];
}

void cpu_io_wait(void)
{
}

static void fixture(void)
{
    for (size_t index = 0U; index < sizeof(cmos); ++index) {
        cmos[index] = 0U;
    }
    cmos[TEST_RTC_STATUS_B] = UINT8_C(0x02); /* BCD, 24-hour. */
    cmos[0x00U] = 0x58U;
    cmos[0x02U] = 0x59U;
    cmos[0x04U] = 0x23U;
    cmos[0x06U] = 0x05U;
    cmos[0x07U] = 0x29U;
    cmos[0x08U] = 0x02U;
    cmos[0x09U] = 0x24U;
    cmos[0x32U] = 0x20U;
    selected_register = 0U;
    seconds_reads = 0U;
    mode = RTC_TEST_STABLE;
    interrupts_enabled = true;
}

int main(void)
{
    struct wall_clock_utc utc;
    int64_t epoch;

    if (!wall_clock_self_test()) {
        return 1;
    }
    fixture();
    if (wall_clock_read_utc(&utc) != WALL_CLOCK_STATUS_OK ||
        utc.year != 2024U || utc.month != 2U || utc.day != 29U ||
        utc.hour != 23U || utc.minute != 59U || utc.second != 58U ||
        !interrupts_enabled ||
        wall_clock_utc_to_unix(&utc, &epoch) != WALL_CLOCK_STATUS_OK ||
        epoch != INT64_C(1709251198)) {
        return 2;
    }

    fixture();
    mode = RTC_TEST_ROLLOVER;
    if (wall_clock_read_utc(&utc) != WALL_CLOCK_STATUS_OK ||
        utc.second != 59U || seconds_reads < 4U) {
        return 3;
    }

    fixture();
    mode = RTC_TEST_UNSTABLE;
    if (wall_clock_read_utc(&utc) != WALL_CLOCK_STATUS_UNSTABLE ||
        !interrupts_enabled) {
        return 4;
    }

    fixture();
    mode = RTC_TEST_UIP_STUCK;
    if (wall_clock_read_utc(&utc) != WALL_CLOCK_STATUS_UPDATE_TIMEOUT ||
        !interrupts_enabled) {
        return 5;
    }

    fixture();
    cmos[0x00U] = 0x6AU;
    return wall_clock_read_utc(&utc) == WALL_CLOCK_STATUS_INVALID_DATA ? 0 : 6;
}
