/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_APIC_H
#define PHIPIA_APIC_H

#include <stdbool.h>
#include <stdint.h>

#include <phipia/acpi.h>

/*
 * Intel SDM volume 3A section 11.9 requires the spurious vector's low four
 * bits to be set on some processor generations, so Phipia fixes it at 0xFF.
 */
#define APIC_SPURIOUS_VECTOR UINT8_C(0xFF)

enum apic_status {
    APIC_STATUS_OK = 0,
    APIC_STATUS_NULL_ARGUMENT,
    APIC_STATUS_ALREADY_ONLINE,
    APIC_STATUS_INTERRUPTS_ENABLED,
    APIC_STATUS_UNSUPPORTED,
    APIC_STATUS_HARDWARE_DISABLED,
    APIC_STATUS_X2APIC_MODE,
    APIC_STATUS_NOT_BOOTSTRAP,
    APIC_STATUS_NULL_BASE,
    APIC_STATUS_BASE_OUTSIDE_EARLY_MAP,
    APIC_STATUS_BASE_DISAGREES_WITH_ACPI,
    APIC_STATUS_EXTERNAL_APIC,
    APIC_STATUS_TOO_FEW_LVT_ENTRIES,
    APIC_STATUS_ID_DISAGREES_WITH_ACPI,
    APIC_STATUS_INTERRUPT_FAILURE,
    APIC_STATUS_READBACK_MISMATCH,
    APIC_STATUS_NOT_ONLINE,
    APIC_STATUS_EOI_BROADCAST_SUPPRESSION_UNSUPPORTED,
    APIC_STATUS_COUNT
};

struct apic_state {
    uint64_t base_address;
    uint32_t id;
    uint8_t version;
    uint8_t max_lvt_entry;
    bool eoi_broadcast_suppression_supported;
    bool eoi_broadcasts_suppressed;
    bool legacy_interrupts_routed;
    bool online;
};

enum apic_status apic_bring_online(const struct acpi_topology *topology);
enum apic_status apic_retire_legacy_routing(void);
enum apic_status apic_suppress_eoi_broadcasts(void);
void apic_send_eoi(void);

/*
 * The local APIC's own register window, for subsystems that are part of this
 * device rather than callers of it. Both require the APIC to be online; an
 * offline APIC has no window and reads back zero rather than faulting.
 */
uint32_t apic_register_read(uint32_t offset);
void apic_register_write(uint32_t offset, uint32_t value);
struct apic_state apic_get_state(void);
bool apic_is_online(void);
uint64_t apic_spurious_count(void);
bool apic_self_test(void);
const char *apic_status_string(enum apic_status status);

#endif
