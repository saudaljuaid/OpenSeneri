/* SPDX-License-Identifier: GPL-3.0-only */
#include <errno.h>
#include <stdint.h>
#include <time.h>

#include <phipia/runtime.h>

_Thread_local int errno;

static long realtime_value = INT64_C(1709251198);
static uint64_t monotonic_value = UINT64_C(1234567890123);

long phipia_realtime_seconds(void)
{
    return realtime_value;
}

uint64_t phipia_monotonic_ns(void)
{
    return monotonic_value;
}

long phipia_sleep_until(uint64_t deadline_ns)
{
    monotonic_value = deadline_ns;
    return 0;
}

int phipia_result(long result)
{
    if (result < 0) {
        errno = (int)-result;
        return -1;
    }
    return (int)result;
}

int main(void)
{
    struct timespec value;
    const struct timespec sleep_interval = {0, 10};
    struct tm broken;
    time_t epoch;

    if (time(&epoch) != INT64_C(1709251198) ||
        epoch != INT64_C(1709251198)) {
        return 1;
    }
    if (clock_gettime(CLOCK_REALTIME, &value) != 0 ||
        value.tv_sec != INT64_C(1709251198) || value.tv_nsec != 0) {
        return 2;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 ||
        value.tv_sec != 1234 || value.tv_nsec != 567890123L) {
        return 3;
    }
    if (nanosleep(&sleep_interval, NULL) != 0 ||
        monotonic_value != UINT64_C(1234567890133)) {
        return 4;
    }
    monotonic_value = UINT64_MAX - 5U;
    errno = 0;
    if (nanosleep(&sleep_interval, NULL) != -1 || errno != EINVAL ||
        monotonic_value != UINT64_MAX - 5U) {
        return 10;
    }
    if (gmtime_r(&epoch, &broken) != &broken ||
        broken.tm_year != 124 || broken.tm_mon != 1 ||
        broken.tm_mday != 29 || broken.tm_hour != 23 ||
        broken.tm_min != 59 || broken.tm_sec != 58 ||
        broken.tm_wday != 4 || broken.tm_yday != 59 ||
        broken.tm_isdst != 0) {
        return 5;
    }
    if (localtime_r(&epoch, &broken) != &broken || broken.tm_hour != 23 ||
        broken.tm_isdst != 0) {
        return 6;
    }

    epoch = INT64_C(253402300799);
    if (gmtime_r(&epoch, &broken) != &broken || broken.tm_year != 8099 ||
        broken.tm_mon != 11 || broken.tm_mday != 31 ||
        broken.tm_hour != 23 || broken.tm_min != 59 || broken.tm_sec != 59) {
        return 7;
    }
    epoch = -1;
    errno = 0;
    if (gmtime_r(&epoch, &broken) != NULL || errno != ERANGE) {
        return 8;
    }
    realtime_value = -PHIPIA_EIO;
    errno = 0;
    return time(NULL) == (time_t)-1 && errno == EIO ? 0 : 9;
}
