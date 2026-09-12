/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_RTC_H
#define PHIPIA_RTC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Wall-clock time, read from the CMOS real-time clock.
 *
 * Phipia's existing clock answers "how long since boot" and nothing else,
 * which is the right answer for scheduling and the wrong one for a taskbar
 * that has to print a date.  This is the only place in the kernel that claims
 * to know what day it is, and it claims it from the one device that was told.
 *
 * The RTC updates its own registers roughly once a second, and a read that
 * straddles that update returns a mixture of two different seconds.  Every
 * read here therefore takes the whole register set twice and keeps the result
 * only when both passes agree, having first waited out any update the chip
 * had already begun.  A read that cannot be made to agree is reported as a
 * failure rather than as a plausible-looking time.
 *
 * Nothing here is interpreted as a time zone.  The chip is read exactly as it
 * is set, which under QEMU's default is UTC, and the caller is told which
 * reading it got rather than being handed a silent conversion.
 */

#define RTC_STATUS_STRING_MAX 48U

enum rtc_status {
    RTC_STATUS_OK = 0,
    RTC_STATUS_NULL_ARGUMENT,
    RTC_STATUS_NOT_STARTED,
    RTC_STATUS_ALREADY_STARTED,
    RTC_STATUS_UPDATE_NEVER_SETTLED,
    RTC_STATUS_UNSTABLE_READ,
    RTC_STATUS_BAD_FIELD
};

struct rtc_time {
    uint16_t year;   /* full year, e.g. 2026 */
    uint8_t month;   /* 1..12 */
    uint8_t day;     /* 1..31 */
    uint8_t hour;    /* 0..23, always normalized out of 12-hour mode */
    uint8_t minute;  /* 0..59 */
    uint8_t second;  /* 0..60, leap second tolerated */
    uint8_t weekday; /* 0 = Sunday .. 6 = Saturday, derived, not read */
};

struct rtc_info {
    bool started;
    bool binary_coded_decimal;
    bool twelve_hour;
    bool century_register_trusted;
    uint8_t century_register;
    uint64_t reads;
    uint64_t retries;
    uint64_t refusals;
};

/*
 * Latch the chip's format from status register B and take one confirming
 * read.  Fails, and stays unstarted, if the chip never settles.
 */
enum rtc_status rtc_start(void);
bool rtc_is_started(void);

/* One fresh, update-safe read of the chip. */
enum rtc_status rtc_read(struct rtc_time *time);

/*
 * The current time without touching the bus on every frame.  The last
 * successful chip read is carried forward by the monotonic clock and the
 * chip is consulted again only once the carry has run for a second or more.
 * A caller that redraws a clock sixty times a second therefore performs one
 * CMOS transaction per second, not sixty.
 */
enum rtc_status rtc_now(struct rtc_time *time);

struct rtc_info rtc_get_info(void);

/* 0 = Sunday. Sakamoto's method, valid for every year this kernel accepts. */
uint8_t rtc_weekday_of(uint16_t year, uint8_t month, uint8_t day);
bool rtc_time_is_valid(const struct rtc_time *time);

bool rtc_self_test(void);
const char *rtc_self_test_failure(void);
const char *rtc_status_string(enum rtc_status status);

#endif
