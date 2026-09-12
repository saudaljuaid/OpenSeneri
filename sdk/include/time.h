/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_TIME_H
#define PHIPIA_TIME_H

#include <stddef.h>
#include <stdint.h>

typedef int64_t time_t;
typedef int64_t clock_t;

#define CLOCKS_PER_SEC 1000000L
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

struct timespec { time_t tv_sec; long tv_nsec; };
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

time_t time(time_t *output);
clock_t clock(void);
int clock_gettime(int identifier, struct timespec *result);
int nanosleep(const struct timespec *request, struct timespec *remaining);
double difftime(time_t end, time_t beginning);
/* UTC conversion accepts Unix seconds from 1970 through 9999; out-of-range
 * values return NULL with errno set to ERANGE. */
struct tm *gmtime(const time_t *value);
struct tm *gmtime_r(const time_t *value, struct tm *result);
struct tm *localtime(const time_t *value);
struct tm *localtime_r(const time_t *value, struct tm *result);
time_t mktime(struct tm *value);
size_t strftime(char *output, size_t capacity, const char *format,
    const struct tm *value);

#endif
