/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NVIDIA_H
#define PHIPIA_NVIDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/pci.h>

/*
 * Fifteen bounded probes for NVIDIA PCI configuration, MMIO registers, and
 * VBIOS data. Sources and per-probe coverage are listed in docs/NVIDIA.md.
 *
 * The QEMU model exercises the bind path. Identity decoding, VBIOS parsing,
 * and non-NVIDIA rejection also run as boot proofs. These probes do not enable
 * bus mastering or allocate DMA. The VBIOS probe temporarily changes the ROM
 * shadow-disable bit and restores it before returning.
 *
 * NOTHING HERE HAS BEEN RUN AGAINST NVIDIA SILICON. The QEMU model validates
 * the emulated register contract; it is not evidence from physical hardware.
 */

/* PCI-SIG vendor identifier assigned to NVIDIA Corporation. */
#define NVIDIA_VENDOR_ID UINT16_C(0x10DE)

#define NVIDIA_DRIVER_COUNT 15U
#define NVIDIA_CONTROLLED_CONTROLS 21U

/*
 * The PROM window is a 64 KiB aperture on every part that has one, and a
 * legal PCI expansion ROM image is a whole number of 512-byte blocks, so this
 * is both the window size and the largest image that can be inside it.
 */
#define NVIDIA_VBIOS_MAX_BYTES 65536U
#define NVIDIA_VBIOS_BLOCK_BYTES 512U

/* How long the timer is watched for movement before it is called stopped. */
#define NVIDIA_TIMER_OBSERVATION_NS UINT64_C(2000000)

enum nvidia_status {
    NVIDIA_STATUS_OK = 0,
    NVIDIA_STATUS_NULL_ARGUMENT,
    NVIDIA_STATUS_BUSY,
    NVIDIA_STATUS_PREREQUISITE,
    NVIDIA_STATUS_CLAIM_FAILURE,
    NVIDIA_STATUS_MAPPING_FAILURE,
    NVIDIA_STATUS_REGISTER_WINDOW,
    NVIDIA_STATUS_ENDIANNESS,
    NVIDIA_STATUS_IDENTITY,
    NVIDIA_STATUS_APERTURE_ALIASED,
    NVIDIA_STATUS_STRAPS,
    NVIDIA_STATUS_APERTURE,
    NVIDIA_STATUS_LINK,
    NVIDIA_STATUS_ENDPOINT,
    NVIDIA_STATUS_POWER_MANAGEMENT,
    NVIDIA_STATUS_MESSAGE_INTERRUPTS,
    NVIDIA_STATUS_EXPANSION_ROM,
    NVIDIA_STATUS_TIMER_SCALE,
    NVIDIA_STATUS_TIMER_STOPPED,
    NVIDIA_STATUS_ROM_ABSENT,
    NVIDIA_STATUS_ROM_MALFORMED,
    NVIDIA_STATUS_ROM_NOT_RESTORED,
    NVIDIA_STATUS_VERSION,
    NVIDIA_STATUS_RELEASE_FAILURE,
    NVIDIA_STATUS_RESOURCE_CENSUS,
    NVIDIA_STATUS_ROBUSTNESS,
    NVIDIA_STATUS_COUNT
};

/*
 * Nouveau's device table keys every part off the top of the master control
 * register, and the families below are that table's own boundaries rather than
 * marketing names: chipset & 0x1f0 is the family, chipset is the part.
 */
enum nvidia_architecture {
    NVIDIA_ARCHITECTURE_UNKNOWN = 0,
    NVIDIA_ARCHITECTURE_CELSIUS,     /* 0x010 */
    NVIDIA_ARCHITECTURE_KELVIN,      /* 0x020 */
    NVIDIA_ARCHITECTURE_RANKINE,     /* 0x030 */
    NVIDIA_ARCHITECTURE_CURIE,       /* 0x040, 0x060 */
    NVIDIA_ARCHITECTURE_TESLA,       /* 0x050, 0x080, 0x090, 0x0a0 */
    NVIDIA_ARCHITECTURE_FERMI,       /* 0x0c0, 0x0d0 */
    NVIDIA_ARCHITECTURE_KEPLER,      /* 0x0e0, 0x0f0, 0x100 */
    NVIDIA_ARCHITECTURE_MAXWELL,     /* 0x110, 0x120 */
    NVIDIA_ARCHITECTURE_PASCAL,      /* 0x130 */
    NVIDIA_ARCHITECTURE_VOLTA,       /* 0x140 */
    NVIDIA_ARCHITECTURE_TURING,      /* 0x160 */
    NVIDIA_ARCHITECTURE_AMPERE,      /* 0x170 */
    NVIDIA_ARCHITECTURE_ADA,         /* 0x190 */
    NVIDIA_ARCHITECTURE_COUNT
};

/*
 * What the master control register says about the part. The decode is pure and
 * is the one piece of this driver that can be, and is, checked without a
 * device present.
 */
struct nvidia_identity {
    uint32_t boot0;
    uint32_t chipset;
    uint32_t implementation;
    uint32_t revision;
    uint32_t family;
    enum nvidia_architecture architecture;
    bool recognized;
};

/*
 * What the VBIOS image in the PROM window turned out to be. This is the exact
 * layout the freestanding Rust validator writes: C never parses these bytes,
 * it only reads the facts Rust extracted from them.
 */
struct nvidia_vbios_image {
    uint32_t image_bytes;
    uint32_t pcir_offset;
    uint32_t bit_offset;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
    uint8_t code_type;
    uint8_t bit_tokens;
    uint8_t bit_token_bytes;
    bool last_image;
};

bool nvidia_vbios_image_layout_self_test(void);

struct nvidia_driver_probe {
    struct pci_address address;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
    /* The device's own identifying value, named per driver in docs/NVIDIA.md */
    uint64_t identity;
    /* One further measured register, also named there. */
    uint64_t detail;
    uint32_t register_reads;
    uint32_t register_writes;
    uint32_t register_bytes;
    bool present;
    bool bound;
};

struct nvidia_result {
    uint32_t declared;
    /* Drivers whose function was present, not distinct functions found. */
    uint32_t present;
    uint32_t bound;
    uint32_t register_reads;
    uint32_t register_writes;
    uint32_t controls;
    /* Which driver refused first, or the table size when none did. */
    uint32_t failed_driver;
    enum nvidia_status failed_status;
    struct nvidia_identity identity;
    struct nvidia_vbios_image vbios;
    bool vbios_valid;
    bool any_function_present;
    bool every_present_function_bound;
    bool teardown_complete;
    bool resource_census_equal;
    struct nvidia_driver_probe probes[NVIDIA_DRIVER_COUNT];
};

/* How a driver reaches its function. */
enum nvidia_access {
    /* Claim the function and map one BAR uncached. */
    NVIDIA_ACCESS_MEMORY = 0,
    /* Claim the function for its BAR descriptions, but map nothing. */
    NVIDIA_ACCESS_APERTURE,
    /* Configuration space only: no claim, no mapping, no register window. */
    NVIDIA_ACCESS_CONFIGURATION,
    NVIDIA_ACCESS_COUNT
};

size_t nvidia_driver_count(void);
const char *nvidia_driver_name(size_t index);
uint8_t nvidia_driver_class(size_t index);
uint8_t nvidia_driver_subclass(size_t index);
uint8_t nvidia_driver_interface(size_t index);
enum nvidia_access nvidia_driver_access(size_t index);
bool nvidia_driver_writes_registers(size_t index);

/* Pure, hardware-free, and checked on every boot. */
struct nvidia_identity nvidia_decode_identity(uint32_t boot0);
const char *nvidia_architecture_name(enum nvidia_architecture architecture);
const uint8_t *nvidia_reference_vbios(size_t *length);

bool nvidia_foundation_self_test(size_t *completed_tests);
enum nvidia_status nvidia_bind(struct nvidia_result *result);
struct nvidia_result nvidia_get_result(void);
bool nvidia_resources_released(void);
const char *nvidia_status_string(enum nvidia_status status);

#endif
