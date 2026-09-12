/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/random.h>

#define CPUID_BASIC UINT32_C(0)
#define CPUID_FEATURES UINT32_C(1)
#define CPUID_EXTENDED UINT32_C(7)
#define CPUID_RDRAND (UINT32_C(1) << 30U)
#define CPUID_RDSEED (UINT32_C(1) << 18U)
#define HARDWARE_ATTEMPTS 16U

static struct random_state state;
static uint64_t generator[4];
static uint64_t last_hardware_word;
static bool last_hardware_word_valid;

static void secure_zero(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;

    while (length != 0U) {
        *bytes++ = 0U;
        --length;
    }
}

static uint64_t rotate_left(uint64_t value, unsigned int count)
{
    return (value << count) | (value >> (64U - count));
}

static uint64_t splitmix64(uint64_t *value)
{
    uint64_t result;

    *value += UINT64_C(0x9E3779B97F4A7C15);
    result = *value;
    result = (result ^ (result >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
    result = (result ^ (result >> 27U)) * UINT64_C(0x94D049BB133111EB);
    return result ^ (result >> 31U);
}

static bool hardware_rdrand(uint64_t *value)
{
    unsigned char success;

    if (value == NULL || !state.rdrand) {
        return false;
    }
    for (size_t attempt = 0U; attempt < HARDWARE_ATTEMPTS; ++attempt) {
        __asm__ volatile (
            "rdrand %0; setc %1"
            : "=r" (*value), "=qm" (success)
            :
            : "cc"
        );
        if (success != 0U) {
            return true;
        }
    }
    return false;
}

static bool hardware_rdseed(uint64_t *value)
{
    unsigned char success;

    if (value == NULL || !state.rdseed) {
        return false;
    }
    for (size_t attempt = 0U; attempt < HARDWARE_ATTEMPTS; ++attempt) {
        __asm__ volatile (
            "rdseed %0; setc %1"
            : "=r" (*value), "=qm" (success)
            :
            : "cc"
        );
        if (success != 0U) {
            return true;
        }
    }
    return false;
}

static bool hardware_strong_word(uint64_t *value)
{
    uint64_t candidate;

    if (value == NULL) {
        return false;
    }
    for (size_t attempt = 0U; attempt < HARDWARE_ATTEMPTS; ++attempt) {
        if (!(hardware_rdseed(&candidate) || hardware_rdrand(&candidate))) {
            continue;
        }
        if (last_hardware_word_valid && candidate == last_hardware_word) {
            continue;
        }
        last_hardware_word = candidate;
        last_hardware_word_valid = true;
        *value = candidate;
        return true;
    }
    return false;
}

static uint64_t generator_next(void)
{
    const uint64_t result = rotate_left(generator[1] * UINT64_C(5), 7U) *
        UINT64_C(9);
    const uint64_t temporary = generator[1] << 17U;

    generator[2] ^= generator[0];
    generator[3] ^= generator[1];
    generator[1] ^= generator[2];
    generator[0] ^= generator[3];
    generator[2] ^= temporary;
    generator[3] = rotate_left(generator[3], 45U);
    return result;
}

void random_initialize(void)
{
    struct cpuid_result basic = {0};
    struct cpuid_result features = {0};
    struct cpuid_result extended = {0};
    uint64_t seed;
    uint64_t hardware = 0U;
    bool hardware_ok = false;

    cpu_cpuid(CPUID_BASIC, 0U, &basic);
    if (basic.eax >= CPUID_FEATURES) {
        cpu_cpuid(CPUID_FEATURES, 0U, &features);
    }
    if (basic.eax >= CPUID_EXTENDED) {
        cpu_cpuid(CPUID_EXTENDED, 0U, &extended);
    }
    state.rdrand = (features.ecx & CPUID_RDRAND) != 0U;
    state.rdseed = (extended.ebx & CPUID_RDSEED) != 0U;

    seed = cpu_read_tsc() ^ clock_monotonic_ns() ^
        (uint64_t)(uintptr_t)(void *)&state ^ UINT64_C(0x5341504F5445524E);
    if (hardware_rdseed(&hardware) || hardware_rdrand(&hardware)) {
        seed ^= hardware;
        hardware_ok = true;
    }
    for (size_t index = 0U; index < 4U; ++index) {
        generator[index] = splitmix64(&seed);
    }
    if ((generator[0] | generator[1] | generator[2] | generator[3]) == 0U) {
        generator[0] = UINT64_C(0x6A09E667F3BCC909);
    }
    state.bytes_issued = 0U;
    state.reseed_count = hardware_ok ? 1U : 0U;
    /* This construction is useful entropy, but is not an audited CSPRNG. */
    state.capability = hardware_ok ? RANDOM_CAPABILITY_INITIALIZED :
        RANDOM_CAPABILITY_DEGRADED;
    last_hardware_word = hardware;
    last_hardware_word_valid = hardware_ok;
    hardware = 0U;
    seed = 0U;
}

enum random_status random_bytes(void *destination, size_t length)
{
    uint8_t *output = destination;
    uint64_t word = 0U;
    size_t available = 0U;

    if (destination == NULL && length != 0U) {
        return RANDOM_STATUS_NULL_ARGUMENT;
    }
    if (length > RANDOM_MAX_REQUEST_BYTES) {
        return RANDOM_STATUS_TOO_LARGE;
    }
    if (state.capability == RANDOM_CAPABILITY_UNAVAILABLE) {
        return RANDOM_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (available == 0U) {
            word = generator_next();
            available = sizeof(word);
        }
        output[index] = (uint8_t)word;
        word >>= 8U;
        --available;
    }
    word = 0U;
    state.bytes_issued += length;
    return RANDOM_STATUS_OK;
}

enum random_status random_strong_bytes(void *destination, size_t length)
{
    uint8_t staging[RANDOM_MAX_REQUEST_BYTES];
    size_t completed = 0U;

    if (destination == NULL && length != 0U) {
        return RANDOM_STATUS_NULL_ARGUMENT;
    }
    if (length > RANDOM_MAX_REQUEST_BYTES) {
        return RANDOM_STATUS_TOO_LARGE;
    }
    if (length == 0U) {
        return RANDOM_STATUS_OK;
    }
    if (state.capability != RANDOM_CAPABILITY_INITIALIZED) {
        return RANDOM_STATUS_NOT_STRONG;
    }
    while (completed < length) {
        uint64_t word;
        size_t chunk = length - completed;

        if (!hardware_strong_word(&word)) {
            state.capability = RANDOM_CAPABILITY_DEGRADED;
            secure_zero(staging, sizeof(staging));
            return RANDOM_STATUS_NOT_STRONG;
        }
        if (chunk > sizeof(word)) {
            chunk = sizeof(word);
        }
        for (size_t index = 0U; index < chunk; ++index) {
            staging[completed + index] = (uint8_t)word;
            word >>= 8U;
        }
        completed += chunk;
    }
    for (size_t index = 0U; index < length; ++index) {
        ((uint8_t *)destination)[index] = staging[index];
    }
    secure_zero(staging, sizeof(staging));
    state.bytes_issued += length;
    ++state.reseed_count;
    return RANDOM_STATUS_OK;
}

uint16_t random_u16(void)
{
    uint16_t value = 0U;

    (void)random_bytes(&value, sizeof(value));
    return value;
}

uint32_t random_u32(void)
{
    uint32_t value = 0U;

    (void)random_bytes(&value, sizeof(value));
    return value;
}

struct random_state random_get_state(void)
{
    return state;
}

bool random_self_test(void)
{
    uint64_t seed = 0U;
    uint64_t first[4];
    uint64_t second[4];

    for (size_t index = 0U; index < 4U; ++index) {
        first[index] = splitmix64(&seed);
    }
    seed = 0U;
    for (size_t index = 0U; index < 4U; ++index) {
        second[index] = splitmix64(&seed);
        if (first[index] != second[index] || first[index] == 0U) {
            return false;
        }
    }
    return random_bytes(NULL, 1U) == RANDOM_STATUS_NULL_ARGUMENT &&
        random_bytes(first, RANDOM_MAX_REQUEST_BYTES + 1U) ==
            RANDOM_STATUS_TOO_LARGE &&
        random_strong_bytes(NULL, 1U) == RANDOM_STATUS_NULL_ARGUMENT &&
        random_strong_bytes(first, RANDOM_MAX_REQUEST_BYTES + 1U) ==
            RANDOM_STATUS_TOO_LARGE;
}

const char *random_capability_string(enum random_capability capability)
{
    switch (capability) {
    case RANDOM_CAPABILITY_UNAVAILABLE: return "unavailable";
    case RANDOM_CAPABILITY_DEGRADED: return "degraded";
    case RANDOM_CAPABILITY_INITIALIZED: return "initialized";
    default: return "unknown";
    }
}

const char *random_status_string(enum random_status status)
{
    static const char *const messages[RANDOM_STATUS_COUNT] = {
        "ok",
        "null random destination",
        "random request exceeds the bounded limit",
        "random source is not initialized",
        "strong hardware entropy is unavailable"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        RANDOM_STATUS_COUNT, "random status messages are out of sync");
    if (status < RANDOM_STATUS_OK || status >= RANDOM_STATUS_COUNT) {
        return "unknown random status";
    }
    return messages[status];
}
