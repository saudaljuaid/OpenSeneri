/* SPDX-License-Identifier: GPL-3.0-only */
#include <time.h>

#include <errno.h>
#include <stddef.h>

#include <phipia/runtime.h>

#define TIME_SECONDS_PER_DAY UINT64_C(86400)
#define TIME_MAX_UNIX_SECONDS INT64_C(253402300799)

static _Thread_local struct tm broken_down_time;

static int leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const unsigned char days[12] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };

    return month == 1 && leap_year(year) ? 29 : days[month];
}

time_t time(time_t *output)
{
    const long result = phipia_realtime_seconds();

    if (result < 0) {
        errno = (int)-result;
        return (time_t)-1;
    }
    if (output != NULL) {
        *output = (time_t)result;
    }
    return (time_t)result;
}
clock_t clock(void) { return (clock_t)(phipia_monotonic_ns() / 1000U); }
int clock_gettime(int identifier, struct timespec *result)
{
    uint64_t now;
    long realtime;

    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (identifier == CLOCK_REALTIME) {
        realtime = phipia_realtime_seconds();
        if (realtime < 0) {
            errno = (int)-realtime;
            return -1;
        }
        result->tv_sec = (time_t)realtime;
        result->tv_nsec = 0;
        return 0;
    }
    if (identifier != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    now = phipia_monotonic_ns();
    result->tv_sec = (time_t)(now / UINT64_C(1000000000));
    result->tv_nsec = (long)(now % UINT64_C(1000000000));
    return 0;
}
int nanosleep(const struct timespec *request, struct timespec *remaining)
{
    uint64_t interval;
    uint64_t now;
    long result;
    if (request == NULL || request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L ||
        (uint64_t)request->tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        errno = EINVAL; return -1;
    }
    interval = (uint64_t)request->tv_sec * UINT64_C(1000000000) +
        (uint64_t)request->tv_nsec;
    now = phipia_monotonic_ns();
    if (interval > UINT64_MAX - now) {
        errno = EINVAL;
        return -1;
    }
    result = phipia_sleep_until(now + interval);
    if (remaining != NULL) { remaining->tv_sec = 0; remaining->tv_nsec = 0; }
    return phipia_result(result);
}
double difftime(time_t end, time_t beginning) { return (double)(end - beginning); }

struct tm *gmtime_r(const time_t *value, struct tm *result)
{
    uint64_t days;
    uint64_t remaining;
    uint64_t year_days;
    int year = 1970;
    int month = 0;
    int year_day;

    if (value == NULL || result == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (*value < 0 || *value > TIME_MAX_UNIX_SECONDS) {
        errno = ERANGE;
        return NULL;
    }

    days = (uint64_t)*value / TIME_SECONDS_PER_DAY;
    remaining = (uint64_t)*value % TIME_SECONDS_PER_DAY;
    result->tm_hour = (int)(remaining / UINT64_C(3600));
    remaining %= UINT64_C(3600);
    result->tm_min = (int)(remaining / UINT64_C(60));
    result->tm_sec = (int)(remaining % UINT64_C(60));
    result->tm_wday = (int)((days + 4U) % 7U);

    for (;;) {
        year_days = leap_year(year) ? 366U : 365U;
        if (days < year_days) {
            break;
        }
        days -= year_days;
        ++year;
    }
    year_day = (int)days;
    while (days >= (uint64_t)days_in_month(year, month)) {
        days -= (uint64_t)days_in_month(year, month);
        ++month;
    }

    result->tm_mday = (int)days + 1;
    result->tm_mon = month;
    result->tm_year = year - 1900;
    result->tm_yday = year_day;
    result->tm_isdst = 0;
    return result;
}

struct tm *gmtime(const time_t *value)
{
    return gmtime_r(value, &broken_down_time);
}

struct tm *localtime_r(const time_t *value, struct tm *result)
{
    /* Phipia has no timezone database yet; local civil time is explicitly UTC. */
    return gmtime_r(value, result);
}

struct tm *localtime(const time_t *value)
{
    return localtime_r(value, &broken_down_time);
}

time_t mktime(struct tm *value) { (void)value; errno = ENOTSUP; return (time_t)-1; }
size_t strftime(char *output, size_t capacity, const char *format, const struct tm *value)
{
    (void)output; (void)capacity; (void)format; (void)value; errno = ENOTSUP; return 0U;
}
