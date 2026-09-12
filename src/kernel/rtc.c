/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/rtc.h>

#define RTC_ADDRESS_PORT UINT16_C(0x70)
#define RTC_DATA_PORT UINT16_C(0x71)

/* Writing bit 7 of the index port masks the non-maskable interrupt.  Every
 * index written here keeps it clear, so the RTC is read without ever leaving
 * the machine deaf to a hardware fault. */
#define RTC_REGISTER_SECOND UINT8_C(0x00)
#define RTC_REGISTER_MINUTE UINT8_C(0x02)
#define RTC_REGISTER_HOUR UINT8_C(0x04)
#define RTC_REGISTER_DAY UINT8_C(0x07)
#define RTC_REGISTER_MONTH UINT8_C(0x08)
#define RTC_REGISTER_YEAR UINT8_C(0x09)
#define RTC_REGISTER_STATUS_A UINT8_C(0x0A)
#define RTC_REGISTER_STATUS_B UINT8_C(0x0B)
#define RTC_REGISTER_CENTURY UINT8_C(0x32)

#define RTC_STATUS_A_UPDATE_IN_PROGRESS UINT8_C(0x80)
#define RTC_STATUS_B_TWENTY_FOUR_HOUR UINT8_C(0x02)
#define RTC_STATUS_B_BINARY UINT8_C(0x04)
#define RTC_HOUR_PM UINT8_C(0x80)

/*
 * Bounded waits, because an absent or wedged chip must fail rather than hang.
 * The update flag is set for at most 2 ms once per second, so a budget of a
 * quarter of a million polls covers it by orders of magnitude even on a
 * machine whose port reads are unusually cheap.
 */
#define RTC_UPDATE_POLL_LIMIT UINT32_C(250000)
#define RTC_AGREEMENT_ATTEMPTS UINT32_C(16)

/* Re-read the chip once the carried time has aged past this. */
#define RTC_CARRY_LIMIT_NS UINT64_C(1000000000)

/* A century byte outside this range is a register that is not a century. */
#define RTC_CENTURY_MINIMUM UINT8_C(19)
#define RTC_CENTURY_MAXIMUM UINT8_C(21)
#define RTC_CENTURY_ASSUMED UINT8_C(20)

static struct rtc_info info;
static const char *self_test_failure = "RTC self-test has not run";
static struct rtc_time carried;
static bool carried_valid;
static uint64_t carried_ns;

static uint8_t rtc_register(uint8_t index)
{
    cpu_out8(RTC_ADDRESS_PORT, index);
    cpu_io_wait();
    return cpu_in8(RTC_DATA_PORT);
}

static bool rtc_update_in_progress(void)
{
    return (rtc_register(RTC_REGISTER_STATUS_A) &
        RTC_STATUS_A_UPDATE_IN_PROGRESS) != 0U;
}

static bool rtc_wait_for_settled_chip(void)
{
    for (uint32_t poll = 0U; poll < RTC_UPDATE_POLL_LIMIT; ++poll) {
        if (!rtc_update_in_progress()) {
            return true;
        }
        cpu_io_wait();
    }
    return false;
}

static uint8_t rtc_from_bcd(uint8_t value)
{
    return (uint8_t)(((value >> 4) & 0x0FU) * 10U + (value & 0x0FU));
}

uint8_t rtc_weekday_of(uint16_t year, uint8_t month, uint8_t day)
{
    /* Sakamoto's table.  March is treated as the first month so that the leap
     * day falls at the end of the year and never inside the table. */
    static const uint8_t offsets[12U] = {
        0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U
    };
    uint32_t adjusted = year;

    if (month < 1U || month > 12U) {
        return 0U;
    }
    if (month < 3U) {
        adjusted -= 1U;
    }
    return (uint8_t)((adjusted + adjusted / 4U - adjusted / 100U +
        adjusted / 400U + offsets[month - 1U] + day) % 7U);
}

static uint8_t rtc_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t lengths[12U] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };
    const bool leap = (year % 4U == 0U && year % 100U != 0U) ||
        year % 400U == 0U;

    if (month < 1U || month > 12U) {
        return 0U;
    }
    if (month == 2U && leap) {
        return 29U;
    }
    return lengths[month - 1U];
}

bool rtc_time_is_valid(const struct rtc_time *time)
{
    if (time == NULL) {
        return false;
    }
    /* A leap second is a real second sixty, so it is accepted; anything past
     * it is a misread register rather than an unusual minute. */
    return time->year >= 1900U && time->year <= 2999U &&
        time->month >= 1U && time->month <= 12U &&
        time->day >= 1U && time->day <= rtc_days_in_month(time->year,
            time->month) &&
        time->hour <= 23U && time->minute <= 59U && time->second <= 60U &&
        time->weekday <= 6U;
}

static bool rtc_times_equal(const struct rtc_time *left,
    const struct rtc_time *right)
{
    return left->year == right->year && left->month == right->month &&
        left->day == right->day && left->hour == right->hour &&
        left->minute == right->minute && left->second == right->second;
}

static void rtc_sample_raw(struct rtc_time *time, uint8_t *century_raw)
{
    time->second = rtc_register(RTC_REGISTER_SECOND);
    time->minute = rtc_register(RTC_REGISTER_MINUTE);
    time->hour = rtc_register(RTC_REGISTER_HOUR);
    time->day = rtc_register(RTC_REGISTER_DAY);
    time->month = rtc_register(RTC_REGISTER_MONTH);
    time->year = rtc_register(RTC_REGISTER_YEAR);
    *century_raw = rtc_register(RTC_REGISTER_CENTURY);
}

static void rtc_decode(struct rtc_time *time, uint8_t century_raw)
{
    const bool pm = info.twelve_hour &&
        (time->hour & RTC_HOUR_PM) != 0U;
    uint8_t century = RTC_CENTURY_ASSUMED;
    uint8_t hour = (uint8_t)(time->hour & (uint8_t)~RTC_HOUR_PM);

    if (info.binary_coded_decimal) {
        time->second = rtc_from_bcd(time->second);
        time->minute = rtc_from_bcd(time->minute);
        hour = rtc_from_bcd(hour);
        time->day = rtc_from_bcd(time->day);
        time->month = rtc_from_bcd(time->month);
        time->year = rtc_from_bcd((uint8_t)time->year);
        century_raw = rtc_from_bcd(century_raw);
    }
    if (info.twelve_hour) {
        /* Twelve o'clock is stored as twelve, not as zero, in both halves of
         * the day; midnight is therefore 12 AM and maps to hour zero. */
        if (hour == 12U) {
            hour = 0U;
        }
        if (pm) {
            hour = (uint8_t)(hour + 12U);
        }
    }
    time->hour = hour;
    if (century_raw >= RTC_CENTURY_MINIMUM &&
            century_raw <= RTC_CENTURY_MAXIMUM) {
        century = century_raw;
        info.century_register_trusted = true;
    } else {
        info.century_register_trusted = false;
    }
    info.century_register = century_raw;
    time->year = (uint16_t)((uint16_t)century * 100U + time->year);
    time->weekday = rtc_weekday_of(time->year, time->month, time->day);
}

static enum rtc_status rtc_read_locked(struct rtc_time *time)
{
    struct rtc_time first;
    struct rtc_time second;
    uint8_t first_century;
    uint8_t second_century;

    if (!rtc_wait_for_settled_chip()) {
        info.refusals += 1U;
        return RTC_STATUS_UPDATE_NEVER_SETTLED;
    }
    rtc_sample_raw(&first, &first_century);
    for (uint32_t attempt = 0U; attempt < RTC_AGREEMENT_ATTEMPTS; ++attempt) {
        if (!rtc_wait_for_settled_chip()) {
            info.refusals += 1U;
            return RTC_STATUS_UPDATE_NEVER_SETTLED;
        }
        rtc_sample_raw(&second, &second_century);
        if (rtc_times_equal(&first, &second) &&
                first_century == second_century) {
            rtc_decode(&second, second_century);
            if (!rtc_time_is_valid(&second)) {
                info.refusals += 1U;
                return RTC_STATUS_BAD_FIELD;
            }
            *time = second;
            info.reads += 1U;
            return RTC_STATUS_OK;
        }
        first = second;
        first_century = second_century;
        info.retries += 1U;
    }
    info.refusals += 1U;
    return RTC_STATUS_UNSTABLE_READ;
}

enum rtc_status rtc_start(void)
{
    struct rtc_time probe;
    uint8_t status_b;

    if (info.started) {
        return RTC_STATUS_ALREADY_STARTED;
    }
    info = (struct rtc_info){ 0 };
    status_b = rtc_register(RTC_REGISTER_STATUS_B);
    info.binary_coded_decimal = (status_b & RTC_STATUS_B_BINARY) == 0U;
    info.twelve_hour = (status_b & RTC_STATUS_B_TWENTY_FOUR_HOUR) == 0U;

    const enum rtc_status status = rtc_read_locked(&probe);

    if (status != RTC_STATUS_OK) {
        return status;
    }
    info.started = true;
    carried = probe;
    carried_valid = true;
    carried_ns = clock_monotonic_ns();
    return RTC_STATUS_OK;
}

bool rtc_is_started(void)
{
    return info.started;
}

enum rtc_status rtc_read(struct rtc_time *time)
{
    if (time == NULL) {
        return RTC_STATUS_NULL_ARGUMENT;
    }
    if (!info.started) {
        return RTC_STATUS_NOT_STARTED;
    }
    const enum rtc_status status = rtc_read_locked(time);

    if (status == RTC_STATUS_OK) {
        carried = *time;
        carried_valid = true;
        carried_ns = clock_monotonic_ns();
    }
    return status;
}

static void rtc_carry_seconds(struct rtc_time *time, uint64_t seconds)
{
    uint64_t remaining = seconds;

    /* A carry only ever advances the clock between two chip reads, so it is
     * bounded in practice by one refresh interval.  It is still written to be
     * correct across a day boundary, because "in practice" is not a bound. */
    if (remaining == 0U) {
        return;
    }
    uint64_t total = (uint64_t)time->hour * 3600U +
        (uint64_t)time->minute * 60U + (uint64_t)time->second + remaining;
    uint64_t days = total / 86400U;

    total %= 86400U;
    time->hour = (uint8_t)(total / 3600U);
    time->minute = (uint8_t)((total % 3600U) / 60U);
    time->second = (uint8_t)(total % 60U);
    while (days-- > 0U) {
        const uint8_t length = rtc_days_in_month(time->year, time->month);

        if (time->day < length) {
            time->day = (uint8_t)(time->day + 1U);
        } else {
            time->day = 1U;
            if (time->month == 12U) {
                time->month = 1U;
                time->year = (uint16_t)(time->year + 1U);
            } else {
                time->month = (uint8_t)(time->month + 1U);
            }
        }
    }
    time->weekday = rtc_weekday_of(time->year, time->month, time->day);
}

enum rtc_status rtc_now(struct rtc_time *time)
{
    if (time == NULL) {
        return RTC_STATUS_NULL_ARGUMENT;
    }
    if (!info.started) {
        return RTC_STATUS_NOT_STARTED;
    }
    const uint64_t now = clock_monotonic_ns();
    const uint64_t elapsed = now >= carried_ns ? now - carried_ns : 0U;

    if (!carried_valid || elapsed >= RTC_CARRY_LIMIT_NS) {
        const enum rtc_status status = rtc_read_locked(time);

        if (status == RTC_STATUS_OK) {
            carried = *time;
            carried_valid = true;
            carried_ns = now;
            return RTC_STATUS_OK;
        }
        if (!carried_valid) {
            return status;
        }
        /* The chip refused this once.  Report the carried time rather than
         * no time at all, and let the next call try the bus again. */
    }
    *time = carried;
    rtc_carry_seconds(time, elapsed / UINT64_C(1000000000));
    return RTC_STATUS_OK;
}

struct rtc_info rtc_get_info(void)
{
    return info;
}

bool rtc_self_test(void)
{
    struct rtc_time time;

    self_test_failure = "RTC self-test passed";
    if (rtc_from_bcd(0x00U) != 0U || rtc_from_bcd(0x09U) != 9U ||
        rtc_from_bcd(0x10U) != 10U || rtc_from_bcd(0x59U) != 59U ||
        rtc_from_bcd(0x99U) != 99U) {
        self_test_failure = "binary-coded decimal conversion is wrong";
        return false;
    }
    /* Known weekdays: the Unix epoch was a Thursday, 2000-01-01 a Saturday,
     * 2024-02-29 a Thursday, and 2100-03-01 a Monday. */
    if (rtc_weekday_of(1970U, 1U, 1U) != 4U ||
        rtc_weekday_of(2000U, 1U, 1U) != 6U ||
        rtc_weekday_of(2024U, 2U, 29U) != 4U ||
        rtc_weekday_of(2100U, 3U, 1U) != 1U) {
        self_test_failure = "weekday derivation is wrong";
        return false;
    }
    if (rtc_days_in_month(2024U, 2U) != 29U ||
        rtc_days_in_month(1900U, 2U) != 28U ||
        rtc_days_in_month(2000U, 2U) != 29U ||
        rtc_days_in_month(2023U, 2U) != 28U ||
        rtc_days_in_month(2023U, 13U) != 0U) {
        self_test_failure = "month length is wrong";
        return false;
    }
    time = (struct rtc_time){ 2026U, 12U, 31U, 23U, 59U, 59U, 0U };
    time.weekday = rtc_weekday_of(time.year, time.month, time.day);
    rtc_carry_seconds(&time, 1U);
    if (time.year != 2027U || time.month != 1U || time.day != 1U ||
        time.hour != 0U || time.minute != 0U || time.second != 0U ||
        time.weekday != rtc_weekday_of(2027U, 1U, 1U)) {
        self_test_failure = "carrying a second across a year is wrong";
        return false;
    }
    time = (struct rtc_time){ 2024U, 2U, 28U, 12U, 0U, 0U, 0U };
    rtc_carry_seconds(&time, 86400U);
    if (time.year != 2024U || time.month != 2U || time.day != 29U ||
        time.hour != 12U) {
        self_test_failure = "carrying a day into a leap day is wrong";
        return false;
    }
    time = (struct rtc_time){ 2026U, 2U, 30U, 0U, 0U, 0U, 0U };
    if (rtc_time_is_valid(&time)) {
        self_test_failure = "an impossible date was accepted";
        return false;
    }
    time = (struct rtc_time){ 2026U, 6U, 15U, 24U, 0U, 0U, 0U };
    if (rtc_time_is_valid(&time)) {
        self_test_failure = "an impossible hour was accepted";
        return false;
    }
    time = (struct rtc_time){ 2026U, 6U, 15U, 23U, 59U, 60U, 1U };
    if (!rtc_time_is_valid(&time)) {
        self_test_failure = "a leap second was refused";
        return false;
    }
    return true;
}

const char *rtc_self_test_failure(void)
{
    return self_test_failure;
}

const char *rtc_status_string(enum rtc_status status)
{
    switch (status) {
    case RTC_STATUS_OK:
        return "ok";
    case RTC_STATUS_NULL_ARGUMENT:
        return "null argument";
    case RTC_STATUS_NOT_STARTED:
        return "real-time clock not started";
    case RTC_STATUS_ALREADY_STARTED:
        return "real-time clock already started";
    case RTC_STATUS_UPDATE_NEVER_SETTLED:
        return "real-time clock never left its update window";
    case RTC_STATUS_UNSTABLE_READ:
        return "real-time clock registers never agreed";
    case RTC_STATUS_BAD_FIELD:
        return "real-time clock reported an impossible field";
    default:
        return "unknown real-time clock status";
    }
}
