/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/wall_clock.h>

#define RTC_INDEX_PORT UINT16_C(0x70)
#define RTC_DATA_PORT UINT16_C(0x71)
#define RTC_SECONDS UINT8_C(0x00)
#define RTC_MINUTES UINT8_C(0x02)
#define RTC_HOURS UINT8_C(0x04)
#define RTC_WEEKDAY UINT8_C(0x06)
#define RTC_DAY UINT8_C(0x07)
#define RTC_MONTH UINT8_C(0x08)
#define RTC_YEAR UINT8_C(0x09)
#define RTC_STATUS_A UINT8_C(0x0A)
#define RTC_STATUS_B UINT8_C(0x0B)
#define RTC_CENTURY UINT8_C(0x32)
#define RTC_UPDATE_IN_PROGRESS UINT8_C(0x80)
#define RTC_UPDATE_INHIBIT UINT8_C(0x80)
#define RTC_BINARY_MODE UINT8_C(0x04)
#define RTC_24_HOUR_MODE UINT8_C(0x02)
#define RTC_PM UINT8_C(0x80)
#define RTC_UPDATE_POLLS 100000U
#define RTC_SAMPLE_ATTEMPTS 8U

struct rtc_sample {
    uint8_t status_b;
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t weekday;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
};

static uint8_t rtc_read_register(uint8_t reg)
{
    /* Bit 7 stays clear: selecting a register must not disable NMI delivery. */
    cpu_out8(RTC_INDEX_PORT, reg);
    cpu_io_wait();
    return cpu_in8(RTC_DATA_PORT);
}

static bool rtc_wait_for_update(void)
{
    for (size_t poll = 0U; poll < RTC_UPDATE_POLLS; ++poll) {
        if ((rtc_read_register(RTC_STATUS_A) & RTC_UPDATE_IN_PROGRESS) == 0U) {
            return true;
        }
    }
    return false;
}

static enum wall_clock_status rtc_read_sample(struct rtc_sample *sample)
{
    if (!rtc_wait_for_update()) {
        return WALL_CLOCK_STATUS_UPDATE_TIMEOUT;
    }

    sample->status_b = rtc_read_register(RTC_STATUS_B);
    sample->second = rtc_read_register(RTC_SECONDS);
    sample->minute = rtc_read_register(RTC_MINUTES);
    sample->hour = rtc_read_register(RTC_HOURS);
    sample->weekday = rtc_read_register(RTC_WEEKDAY);
    sample->day = rtc_read_register(RTC_DAY);
    sample->month = rtc_read_register(RTC_MONTH);
    sample->year = rtc_read_register(RTC_YEAR);
    sample->century = rtc_read_register(RTC_CENTURY);

    /* A rollover that began during the register reads invalidates this sample. */
    return (rtc_read_register(RTC_STATUS_A) & RTC_UPDATE_IN_PROGRESS) == 0U ?
        WALL_CLOCK_STATUS_OK : WALL_CLOCK_STATUS_UNSTABLE;
}

static bool rtc_samples_equal(
    const struct rtc_sample *left,
    const struct rtc_sample *right
)
{
    return left->status_b == right->status_b &&
        left->second == right->second && left->minute == right->minute &&
        left->hour == right->hour && left->weekday == right->weekday &&
        left->day == right->day && left->month == right->month &&
        left->year == right->year && left->century == right->century;
}

static bool bcd_decode(uint8_t input, uint8_t *output)
{
    const uint8_t low = input & UINT8_C(0x0F);
    const uint8_t high = input >> 4U;

    if (low > 9U || high > 9U || output == NULL) {
        return false;
    }
    *output = (uint8_t)(high * 10U + low);
    return true;
}

static bool leap_year(uint16_t year)
{
    return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[12] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };

    if (month == 0U || month > 12U) {
        return 0U;
    }
    if (month == 2U && leap_year(year)) {
        return 29U;
    }
    return days[month - 1U];
}

static enum wall_clock_status validate_utc(const struct wall_clock_utc *utc)
{
    if (utc == NULL) {
        return WALL_CLOCK_STATUS_NULL_ARGUMENT;
    }
    if (utc->year < WALL_CLOCK_MIN_YEAR || utc->year > WALL_CLOCK_MAX_YEAR) {
        return WALL_CLOCK_STATUS_RANGE;
    }
    if (utc->month == 0U || utc->month > 12U || utc->day == 0U ||
        utc->day > days_in_month(utc->year, utc->month) ||
        utc->hour > 23U || utc->minute > 59U || utc->second > 59U ||
        utc->weekday == 0U || utc->weekday > 7U) {
        return WALL_CLOCK_STATUS_INVALID_DATA;
    }
    return WALL_CLOCK_STATUS_OK;
}

static enum wall_clock_status decode_sample(
    const struct rtc_sample *sample,
    struct wall_clock_utc *result
)
{
    struct wall_clock_utc decoded;
    enum wall_clock_status status;
    uint8_t century;
    uint8_t year;
    uint8_t encoded_hour;
    uint8_t hour;
    bool binary;
    bool hour_24;
    bool pm;

    if (sample == NULL || result == NULL) {
        return WALL_CLOCK_STATUS_NULL_ARGUMENT;
    }

    binary = (sample->status_b & RTC_BINARY_MODE) != 0U;
    hour_24 = (sample->status_b & RTC_24_HOUR_MODE) != 0U;
    pm = (sample->hour & RTC_PM) != 0U;

    if ((sample->status_b & RTC_UPDATE_INHIBIT) != 0U) {
        return WALL_CLOCK_STATUS_INVALID_DATA;
    }

    encoded_hour = sample->hour & (uint8_t)~RTC_PM;
    if (binary) {
        decoded.second = sample->second;
        decoded.minute = sample->minute;
        decoded.weekday = sample->weekday;
        decoded.day = sample->day;
        decoded.month = sample->month;
        year = sample->year;
        century = sample->century;
        hour = encoded_hour;
    } else if (!bcd_decode(sample->second, &decoded.second) ||
        !bcd_decode(sample->minute, &decoded.minute) ||
        !bcd_decode(sample->weekday, &decoded.weekday) ||
        !bcd_decode(sample->day, &decoded.day) ||
        !bcd_decode(sample->month, &decoded.month) ||
        !bcd_decode(sample->year, &year) ||
        !bcd_decode(sample->century, &century) ||
        !bcd_decode(encoded_hour, &hour)) {
        return WALL_CLOCK_STATUS_INVALID_DATA;
    }

    if (hour_24) {
        if (pm || hour > 23U) {
            return WALL_CLOCK_STATUS_INVALID_DATA;
        }
        decoded.hour = hour;
    } else {
        if (hour == 0U || hour > 12U) {
            return WALL_CLOCK_STATUS_INVALID_DATA;
        }
        decoded.hour = (uint8_t)(hour % 12U + (pm ? 12U : 0U));
    }

    decoded.year = (uint16_t)((uint16_t)century * 100U + year);
    status = validate_utc(&decoded);
    if (status == WALL_CLOCK_STATUS_OK) {
        *result = decoded;
    }
    return status;
}

enum wall_clock_status wall_clock_read_utc(struct wall_clock_utc *result)
{
    struct rtc_sample first;
    struct rtc_sample second;
    enum wall_clock_status status = WALL_CLOCK_STATUS_UNSTABLE;
    bool restore_interrupts;

    if (result == NULL) {
        return WALL_CLOCK_STATUS_NULL_ARGUMENT;
    }
    restore_interrupts = cpu_interrupts_enabled();
    if (restore_interrupts) {
        cpu_interrupt_disable();
    }

    for (size_t attempt = 0U; attempt < RTC_SAMPLE_ATTEMPTS; ++attempt) {
        status = rtc_read_sample(&first);
        if (status == WALL_CLOCK_STATUS_UPDATE_TIMEOUT) {
            break;
        }
        if (status != WALL_CLOCK_STATUS_OK) {
            continue;
        }
        status = rtc_read_sample(&second);
        if (status == WALL_CLOCK_STATUS_UPDATE_TIMEOUT) {
            break;
        }
        if (status == WALL_CLOCK_STATUS_OK && rtc_samples_equal(&first, &second)) {
            status = decode_sample(&second, result);
            break;
        }
        status = WALL_CLOCK_STATUS_UNSTABLE;
    }

    if (restore_interrupts) {
        cpu_interrupt_enable();
    }
    return status;
}

static uint64_t leap_days_through(uint16_t year)
{
    return (uint64_t)year / 4U - (uint64_t)year / 100U +
        (uint64_t)year / 400U;
}

enum wall_clock_status wall_clock_utc_to_unix(
    const struct wall_clock_utc *utc,
    int64_t *seconds
)
{
    uint64_t days;
    enum wall_clock_status status;

    if (seconds == NULL) {
        return WALL_CLOCK_STATUS_NULL_ARGUMENT;
    }
    status = validate_utc(utc);
    if (status != WALL_CLOCK_STATUS_OK) {
        return status;
    }

    days = (uint64_t)(utc->year - WALL_CLOCK_MIN_YEAR) * UINT64_C(365) +
        leap_days_through((uint16_t)(utc->year - 1U)) -
        leap_days_through((uint16_t)(WALL_CLOCK_MIN_YEAR - 1U));
    for (uint8_t month = 1U; month < utc->month; ++month) {
        days += days_in_month(utc->year, month);
    }
    days += utc->day - 1U;
    *seconds = (int64_t)(days * UINT64_C(86400) +
        (uint64_t)utc->hour * UINT64_C(3600) +
        (uint64_t)utc->minute * UINT64_C(60) + utc->second);
    return WALL_CLOCK_STATUS_OK;
}

enum wall_clock_status wall_clock_read_unix_seconds(int64_t *seconds)
{
    struct wall_clock_utc utc;
    enum wall_clock_status status;

    if (seconds == NULL) {
        return WALL_CLOCK_STATUS_NULL_ARGUMENT;
    }
    status = wall_clock_read_utc(&utc);
    return status == WALL_CLOCK_STATUS_OK ?
        wall_clock_utc_to_unix(&utc, seconds) : status;
}

static bool decode_self_test(
    struct rtc_sample sample,
    uint16_t year,
    uint8_t month,
    uint8_t day,
    uint8_t hour,
    int64_t epoch
)
{
    struct wall_clock_utc utc;
    int64_t actual_epoch;

    return decode_sample(&sample, &utc) == WALL_CLOCK_STATUS_OK &&
        utc.year == year && utc.month == month && utc.day == day &&
        utc.hour == hour &&
        wall_clock_utc_to_unix(&utc, &actual_epoch) == WALL_CLOCK_STATUS_OK &&
        actual_epoch == epoch;
}

bool wall_clock_self_test(void)
{
    const struct rtc_sample bcd_leap = {
        RTC_24_HOUR_MODE, 0x58U, 0x59U, 0x23U, 0x05U, 0x29U, 0x02U,
        0x24U, 0x20U
    };
    const struct rtc_sample binary_midnight = {
        RTC_BINARY_MODE, 0U, 0U, 12U, 7U, 1U, 1U, 0U, 20U
    };
    const struct rtc_sample binary_noon = {
        RTC_BINARY_MODE, 0U, 0U, (uint8_t)(RTC_PM | 12U), 7U, 1U, 1U,
        0U, 20U
    };
    struct rtc_sample invalid = bcd_leap;
    struct wall_clock_utc utc;

    if (!decode_self_test(bcd_leap, 2024U, 2U, 29U, 23U,
            INT64_C(1709251198)) ||
        !decode_self_test(binary_midnight, 2000U, 1U, 1U, 0U,
            INT64_C(946684800)) ||
        !decode_self_test(binary_noon, 2000U, 1U, 1U, 12U,
            INT64_C(946728000))) {
        return false;
    }

    invalid.year = 0x00U;
    invalid.century = 0x21U;
    invalid.day = 0x29U;
    if (decode_sample(&invalid, &utc) != WALL_CLOCK_STATUS_INVALID_DATA) {
        return false;
    }
    invalid = bcd_leap;
    invalid.second = 0x6AU;
    if (decode_sample(&invalid, &utc) != WALL_CLOCK_STATUS_INVALID_DATA) {
        return false;
    }
    invalid = binary_midnight;
    invalid.hour = 0U;
    if (decode_sample(&invalid, &utc) != WALL_CLOCK_STATUS_INVALID_DATA) {
        return false;
    }
    invalid = binary_midnight;
    invalid.status_b |= RTC_UPDATE_INHIBIT;
    if (decode_sample(&invalid, &utc) != WALL_CLOCK_STATUS_INVALID_DATA) {
        return false;
    }
    invalid = binary_midnight;
    invalid.year = 69U;
    invalid.century = 19U;
    return decode_sample(&invalid, &utc) == WALL_CLOCK_STATUS_RANGE &&
        wall_clock_read_utc(NULL) == WALL_CLOCK_STATUS_NULL_ARGUMENT &&
        wall_clock_utc_to_unix(NULL, NULL) ==
            WALL_CLOCK_STATUS_NULL_ARGUMENT;
}

const char *wall_clock_status_string(enum wall_clock_status status)
{
    switch (status) {
    case WALL_CLOCK_STATUS_OK:
        return "ok";
    case WALL_CLOCK_STATUS_NULL_ARGUMENT:
        return "wall clock received a null argument";
    case WALL_CLOCK_STATUS_UPDATE_TIMEOUT:
        return "RTC update did not finish";
    case WALL_CLOCK_STATUS_UNSTABLE:
        return "RTC did not provide two coherent samples";
    case WALL_CLOCK_STATUS_INVALID_DATA:
        return "RTC returned an invalid UTC date";
    case WALL_CLOCK_STATUS_RANGE:
        return "RTC UTC date is outside the supported range";
    default:
        return "unknown wall clock status";
    }
}
