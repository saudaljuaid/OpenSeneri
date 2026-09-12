/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_RANDOM_H
#define PHIPIA_RANDOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RANDOM_MAX_REQUEST_BYTES 256U

enum random_capability {
    RANDOM_CAPABILITY_UNAVAILABLE = 0,
    RANDOM_CAPABILITY_DEGRADED,
    RANDOM_CAPABILITY_INITIALIZED
};

enum random_status {
    RANDOM_STATUS_OK = 0,
    RANDOM_STATUS_NULL_ARGUMENT,
    RANDOM_STATUS_TOO_LARGE,
    RANDOM_STATUS_NOT_INITIALIZED,
    RANDOM_STATUS_NOT_STRONG,
    RANDOM_STATUS_COUNT
};

struct random_state {
    enum random_capability capability;
    uint64_t bytes_issued;
    uint64_t reseed_count;
    bool rdrand;
    bool rdseed;
};

void random_initialize(void);
enum random_status random_bytes(void *destination, size_t length);
enum random_status random_strong_bytes(void *destination, size_t length);
uint16_t random_u16(void);
uint32_t random_u32(void);
struct random_state random_get_state(void);
bool random_self_test(void);
const char *random_capability_string(enum random_capability capability);
const char *random_status_string(enum random_status status);

#endif
