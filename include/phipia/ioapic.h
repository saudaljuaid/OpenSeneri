/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_IOAPIC_H
#define PHIPIA_IOAPIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/acpi.h>

/*
 * Intel 82093AA section 3.2.2 reports the I/O APIC version in the low byte of
 * register 0x01. The directed end-of-interrupt register at memory offset 0x40
 * exists from version 0x20 onwards. Older units still receive the normal local-
 * APIC EOI broadcast, so only directed-mode selection and writes to offset 0x40
 * are gated on this threshold; level-triggered routing itself remains valid.
 */
#define IOAPIC_VERSION_DIRECTED_EOI UINT8_C(0x20)

enum ioapic_status {
    IOAPIC_STATUS_OK = 0,
    IOAPIC_STATUS_NULL_ARGUMENT,
    IOAPIC_STATUS_ALREADY_INITIALIZED,
    IOAPIC_STATUS_NOT_INITIALIZED,
    IOAPIC_STATUS_INTERRUPTS_ENABLED,
    IOAPIC_STATUS_MISSING_IO_APIC,
    IOAPIC_STATUS_ID_DISAGREES_WITH_ACPI,
    IOAPIC_STATUS_TOO_FEW_ENTRIES,
    IOAPIC_STATUS_OVERLAPPING_INTERRUPT_BASE,
    IOAPIC_STATUS_BAD_IRQ,
    IOAPIC_STATUS_BAD_VECTOR,
    IOAPIC_STATUS_BAD_DESTINATION,
    IOAPIC_STATUS_BAD_TRIGGER,
    IOAPIC_STATUS_RESERVED_POLARITY,
    IOAPIC_STATUS_RESERVED_TRIGGER,
    IOAPIC_STATUS_UNROUTABLE_INTERRUPT,
    IOAPIC_STATUS_NO_DIRECTED_EOI,
    IOAPIC_STATUS_VECTOR_IN_USE,
    IOAPIC_STATUS_VECTOR_NOT_ROUTED,
    IOAPIC_STATUS_NOT_LEVEL_TRIGGERED,
    IOAPIC_STATUS_READBACK_MISMATCH,
    IOAPIC_STATUS_LOCAL_EOI_SUPPRESSION_FAILURE,
    IOAPIC_STATUS_COUNT
};

/*
 * Which trigger mode a routing request asks for. Firmware's declaration is the
 * production answer and the only one the boot path uses for a real device.
 *
 * The forced level mode exists because this kernel owns exactly one interrupt
 * source, the 8254, and firmware declares its line edge triggered. Trigger mode
 * is a property of how the I/O APIC samples a pin rather than of the pin, so
 * routing that source level triggered is what makes the level path executable
 * at all before there is a device driver to supply a real one.
 */
enum ioapic_trigger {
    IOAPIC_TRIGGER_FIRMWARE = 0,
    IOAPIC_TRIGGER_FORCE_LEVEL
};

struct ioapic_unit {
    uint32_t identifier;
    uint32_t address;
    uint32_t interrupt_base;
    uint8_t version;
    uint8_t entry_count;
    bool directed_eoi;
};

struct ioapic_state {
    size_t count;
    size_t level_routes;
    bool directed_eoi_mode;
    uint64_t directed_eoi_count;
    uint64_t remote_irr_observed;
    uint64_t remote_irr_missing;
    struct ioapic_unit units[ACPI_MAX_IO_APICS];
};

/*
 * One redirection entry, decomposed into the fields Phipia reasons about, and
 * the two that say which pin on which unit it is. Remote IRR is the device's to
 * set: it latches on every level-triggered delivery and only an end-of-
 * interrupt indication puts it down, either as the normal local-APIC broadcast
 * or through the optional directed-EOI register after broadcasts were
 * suppressed.
 */
struct ioapic_redirection {
    uint32_t unit_identifier;
    uint32_t global_interrupt;
    uint8_t vector;
    uint8_t destination;
    bool active_low;
    bool level_triggered;
    bool masked;
    bool remote_irr;
};

enum ioapic_status ioapic_initialize(const struct acpi_topology *topology);

/* Route an ISA IRQ exactly as firmware declared it. */
enum ioapic_status ioapic_route_isa_irq(
    uint8_t irq,
    uint8_t vector,
    uint32_t destination
);

enum ioapic_status ioapic_route_isa_irq_as(
    uint8_t irq,
    uint8_t vector,
    uint32_t destination,
    enum ioapic_trigger trigger
);

enum ioapic_status ioapic_mask_isa_irq(uint8_t irq);

/*
 * Complete a level-triggered delivery. The live remote IRR is observed first,
 * then the local APIC EOI is sent. If local EOI broadcasts were explicitly
 * suppressed, the vector is finally written to the owning I/O APIC's directed
 * EOI register. Edge-triggered and unrouted vectors are refused by name.
 */
enum ioapic_status ioapic_send_eoi(uint8_t vector);

bool ioapic_vector_is_level_triggered(uint8_t vector);

/* Read a routed vector's live redirection entry back off the hardware. */
enum ioapic_status ioapic_read_redirection(
    uint8_t vector,
    struct ioapic_redirection *entry
);

struct ioapic_state ioapic_get_state(void);
bool ioapic_is_initialized(void);
bool ioapic_self_test(void);
const char *ioapic_status_string(enum ioapic_status status);

#endif
