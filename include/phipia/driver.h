/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_DRIVER_H
#define PHIPIA_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/pci.h>

/*
 * Thirteen bounded drivers for thirteen real devices.
 *
 * Phipia already drives three controllers - xHCI, NVMe and virtio-net - and
 * each of them owns a whole file, because each of them moves data. These
 * thirteen do not: every one of them binds its device, brings it to a defined state,
 * reads the registers that identify it, checks what it read against the
 * device's own specification, and gives everything back. That is the part of a
 * driver that has to be right before anything else can be, and it is the part
 * a kernel needs thirteen times over before it needs any one of them
 * completely.
 *
 * No driver here enables bus mastering, so none of them can reach memory.
 * Phipia has no IOMMU; a driver that only reads registers is a driver that
 * cannot be talked into writing somewhere it should not.
 */
#define DRIVER_MATRIX_CAPACITY 13U
#define DRIVER_MATRIX_RESET_TIMEOUT_NS UINT64_C(1000000000)
#define DRIVER_MATRIX_CONTROLLED_CONTROLS 12U

/* How a driver reaches its device. */
enum driver_access {
    /* Configuration space only: the device has no register window to map. */
    DRIVER_ACCESS_CONFIGURATION = 0,
    /* One memory BAR, claimed and mapped uncached through the substrate. */
    DRIVER_ACCESS_MEMORY,
    DRIVER_ACCESS_COUNT
};

enum driver_status {
    DRIVER_STATUS_OK = 0,
    DRIVER_STATUS_NULL_ARGUMENT,
    DRIVER_STATUS_BUSY,
    DRIVER_STATUS_PREREQUISITE,
    DRIVER_STATUS_MATRIX_INVALID,
    DRIVER_STATUS_ABSENT,
    DRIVER_STATUS_CLAIM_FAILURE,
    DRIVER_STATUS_MAPPING_FAILURE,
    DRIVER_STATUS_REGISTER_WINDOW,
    DRIVER_STATUS_CONFIGURATION_READ,
    DRIVER_STATUS_RESET_TIMEOUT,
    DRIVER_STATUS_IDENTITY,
    DRIVER_STATUS_RELEASE_FAILURE,
    DRIVER_STATUS_RESOURCE_CENSUS,
    DRIVER_STATUS_ROBUSTNESS,
    DRIVER_STATUS_COUNT
};

/* What one driver measured, in the device's own terms. */
struct driver_probe {
    struct pci_address address;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint8_t class_code;
    uint8_t subclass;
    /* The device's own identifying value: a MAC, a chip ID, a version. */
    uint64_t identity;
    /* One further measured register, named per driver in docs/DRIVERS.md. */
    uint64_t detail;
    uint32_t register_reads;
    uint32_t register_writes;
    uint32_t register_bytes;
    bool reset_observed;
    bool present;
    bool bound;
};

struct driver_matrix_result {
    uint32_t declared;
    uint32_t present;
    uint32_t bound;
    uint32_t resets;
    uint32_t register_reads;
    uint32_t register_writes;
    uint32_t controls;
    /* Which driver refused first, or the matrix size when none did. */
    uint32_t failed_driver;
    enum driver_status failed_status;
    bool every_present_device_bound;
    bool teardown_complete;
    bool resource_census_equal;
    struct driver_probe probes[DRIVER_MATRIX_CAPACITY];
};

size_t driver_matrix_count(void);
const char *driver_matrix_name(size_t index);
uint16_t driver_matrix_vendor(size_t index);
uint16_t driver_matrix_device(size_t index);
enum driver_access driver_matrix_access(size_t index);
bool driver_matrix_defines_reset(size_t index);

bool driver_matrix_self_test(size_t *completed_tests);
enum driver_status driver_matrix_bind(struct driver_matrix_result *result);
struct driver_matrix_result driver_matrix_get_result(void);
bool driver_matrix_resources_released(void);
const char *driver_status_string(enum driver_status status);

#endif
