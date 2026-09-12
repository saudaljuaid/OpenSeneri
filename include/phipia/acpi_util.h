/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ACPI_UTIL_H
#define PHIPIA_ACPI_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ACPI 6.6 sections 5.2.6 and 5.2.12 define these wire sizes. */
#define ACPI_SDT_HEADER_SIZE 36U
#define ACPI_MADT_FIXED_SIZE 44U

/*
 * Phipia early-boot policy bound. Firmware controls every table length in this
 * path, so no single table may exceed 1 MiB until the virtual-memory manager
 * can map firmware tables individually. This is not an ACPI limit.
 */
#define ACPI_MAX_TABLE_SIZE (1024U * 1024U)

/*
 * Byte-level primitives shared by every firmware-table reader. The kernel is
 * freestanding, so ACPI parsing has no host libc, and firmware structures are
 * packed and little-endian regardless of their placement.
 */
bool acpi_span_is_early_mapped(uint64_t address, uint64_t length);
uint8_t acpi_byte_sum(const void *data, size_t size);
bool acpi_bytes_equal(const void *left, const void *right, size_t size);
void acpi_bytes_zero(void *data, size_t size);
uint16_t acpi_read_u16(const uint8_t *bytes);
uint32_t acpi_read_u32(const uint8_t *bytes);
uint64_t acpi_read_u64(const uint8_t *bytes);
void acpi_copy_string(char *destination, const char *source, size_t length);

#endif
