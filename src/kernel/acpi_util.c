/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/acpi_util.h>
#include <phipia/boot.h>

bool acpi_span_is_early_mapped(uint64_t address, uint64_t length)
{
    return address < PHIPIA_EARLY_PHYSICAL_LIMIT &&
        length <= PHIPIA_EARLY_PHYSICAL_LIMIT - address;
}

uint8_t acpi_byte_sum(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t sum = 0U;

    for (size_t index = 0; index < size; ++index) {
        sum = (uint8_t)(sum + bytes[index]);
    }

    return sum;
}

bool acpi_bytes_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *left_bytes = (const uint8_t *)left;
    const uint8_t *right_bytes = (const uint8_t *)right;

    for (size_t index = 0; index < size; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return false;
        }
    }

    return true;
}

void acpi_bytes_zero(void *data, size_t size)
{
    uint8_t *bytes = (uint8_t *)data;

    for (size_t index = 0; index < size; ++index) {
        bytes[index] = 0U;
    }
}

uint16_t acpi_read_u16(const uint8_t *bytes)
{
    uint16_t value = bytes[0];

    value = (uint16_t)(value | ((uint16_t)bytes[1] << 8U));
    return value;
}

uint32_t acpi_read_u32(const uint8_t *bytes)
{
    uint32_t value = bytes[0];

    value |= (uint32_t)bytes[1] << 8U;
    value |= (uint32_t)bytes[2] << 16U;
    value |= (uint32_t)bytes[3] << 24U;
    return value;
}

uint64_t acpi_read_u64(const uint8_t *bytes)
{
    uint64_t value = acpi_read_u32(bytes);

    value |= (uint64_t)acpi_read_u32(bytes + sizeof(uint32_t)) << 32U;
    return value;
}

void acpi_copy_string(char *destination, const char *source, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        destination[index] = source[index];
    }

    destination[length] = '\0';
}
