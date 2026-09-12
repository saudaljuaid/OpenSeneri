/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/acpi.h>
#include <phipia/acpi_util.h>
#include <phipia/apic.h>
#include <phipia/cpu.h>
#include <phipia/interrupts.h>
#include <phipia/ioapic.h>

/*
 * Intel 82093AA section 3.1: the I/O APIC is reached through an index register
 * and a data window sixteen bytes above it, not as a flat register file.
 */
#define IOAPIC_REGISTER_SELECT UINT32_C(0x00)
#define IOAPIC_REGISTER_WINDOW UINT32_C(0x10)

/*
 * The directed end-of-interrupt register. Intel SDM volume 3A section 11.5.5
 * and the I/O APIC datasheet place it at memory offset 0x40, and it is a
 * register in its own right rather than an index the window pair selects.
 * Writing a vector to it clears the remote IRR of every redirection entry
 * carrying that vector. It exists from I/O APIC version 0x20 onwards, which is
 * why every unit's version is checked before a level-triggered entry is
 * programmed against it.
 */
#define IOAPIC_REGISTER_EOI UINT32_C(0x40)

/* Intel 82093AA sections 3.2.1 through 3.2.4 assign these register indices. */
#define IOAPIC_INDEX_ID UINT32_C(0x00)
#define IOAPIC_INDEX_VERSION UINT32_C(0x01)
#define IOAPIC_INDEX_REDIRECTION UINT32_C(0x10)

/*
 * Intel 82093AA section 3.2.4 defines the redirection entry fields. Delivery
 * status at bit 12 and remote IRR at bit 14 are read-only status the I/O APIC
 * owns; everything else in the low word is Phipia's to write.
 */
#define IOAPIC_DELIVERY_FIXED UINT32_C(0x00000000)
#define IOAPIC_DESTINATION_PHYSICAL UINT32_C(0x00000000)
#define IOAPIC_VECTOR_MASK UINT32_C(0x000000FF)
#define IOAPIC_DELIVERY_MODE_MASK UINT32_C(0x00000700)
#define IOAPIC_DESTINATION_LOGICAL UINT32_C(0x00000800)
#define IOAPIC_DELIVERY_STATUS UINT32_C(0x00001000)
#define IOAPIC_POLARITY_ACTIVE_LOW UINT32_C(0x00002000)
#define IOAPIC_REMOTE_IRR UINT32_C(0x00004000)
#define IOAPIC_TRIGGER_LEVEL UINT32_C(0x00008000)
#define IOAPIC_MASKED UINT32_C(0x00010000)
#define IOAPIC_DESTINATION_SHIFT 24U
#define IOAPIC_ENTRY_WRITABLE_MASK \
    (IOAPIC_VECTOR_MASK | IOAPIC_DELIVERY_MODE_MASK | \
     IOAPIC_DESTINATION_LOGICAL | IOAPIC_POLARITY_ACTIVE_LOW | \
     IOAPIC_TRIGGER_LEVEL | IOAPIC_MASKED)

/*
 * ACPI 6.6 table 5.25 encodes polarity in bits 0-1 and trigger mode in bits
 * 2-3. Both fields reserve one encoding, and a reserved encoding describes
 * electricals Phipia cannot program, so it is refused rather than read as the
 * nearest thing that fits. Conforming means whatever the bus specifies, and
 * ACPI 6.6 section 5.2.12.5 makes that edge triggered and active high for ISA.
 */
#define MPS_INTI_POLARITY_MASK UINT16_C(0x0003)
#define MPS_INTI_POLARITY_CONFORMING UINT16_C(0x0000)
#define MPS_INTI_POLARITY_ACTIVE_HIGH UINT16_C(0x0001)
#define MPS_INTI_POLARITY_RESERVED UINT16_C(0x0002)
#define MPS_INTI_POLARITY_ACTIVE_LOW UINT16_C(0x0003)
#define MPS_INTI_TRIGGER_MASK UINT16_C(0x000C)
#define MPS_INTI_TRIGGER_CONFORMING UINT16_C(0x0000)
#define MPS_INTI_TRIGGER_EDGE UINT16_C(0x0004)
#define MPS_INTI_TRIGGER_RESERVED UINT16_C(0x0008)
#define MPS_INTI_TRIGGER_LEVEL UINT16_C(0x000C)

#define IOAPIC_ISA_IRQ_COUNT 16U
#define IOAPIC_MIN_ENTRIES 16U
#define IOAPIC_ROUTABLE_VECTORS \
    ((size_t)(INTERRUPT_IOAPIC_LIMIT - INTERRUPT_IOAPIC_BASE))

/*
 * The delivery window holds exactly one vector per ISA interrupt, so a vector
 * that passes the range check always has a route record and the table can never
 * be overrun. That bound is proved here rather than defended by a branch no
 * test could reach.
 */
_Static_assert(IOAPIC_ROUTABLE_VECTORS == IOAPIC_ISA_IRQ_COUNT,
               "one I/O APIC delivery vector per ISA interrupt");
_Static_assert(ACPI_MAX_IO_APICS <= UINT8_MAX,
               "a route record must be able to name any discovered unit");
_Static_assert(IOAPIC_ENTRY_WRITABLE_MASK == UINT32_C(0x0001AFFF),
               "redirection entry writable mask changed");
_Static_assert((IOAPIC_ENTRY_WRITABLE_MASK &
                (IOAPIC_DELIVERY_STATUS | IOAPIC_REMOTE_IRR)) == 0U,
               "read-only entry bits must never be written or compared");

/* What firmware says an ISA interrupt is, once its overrides are applied. */
struct ioapic_source {
    uint32_t global_interrupt;
    bool active_low;
    bool level_triggered;
};

/*
 * Where a delivery vector currently lands. The dispatcher has only the vector
 * when an interrupt arrives, and a directed end of interrupt has to reach the
 * unit that owns the entry, so the mapping is recorded when the entry is
 * programmed rather than searched for afterwards.
 */
struct ioapic_route {
    bool routed;
    bool level_triggered;
    uint8_t unit;
    uint8_t entry;
};

static struct ioapic_state state;
static struct ioapic_route routes[IOAPIC_ROUTABLE_VECTORS];
static bool initialized;

/*
 * The index and window registers are memory-mapped device state: each access
 * must reach the device exactly once and in program order, which is what
 * volatile expresses. A select followed by a window access is one indivisible
 * transaction, so every caller below runs with interrupts disabled.
 */
static uint32_t ioapic_read(const struct ioapic_unit *unit, uint32_t index)
{
    volatile uint32_t *select = (volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_SELECT);
    const volatile uint32_t *window = (const volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_WINDOW);

    *select = index;
    return *window;
}

static void ioapic_write(
    const struct ioapic_unit *unit,
    uint32_t index,
    uint32_t value
)
{
    volatile uint32_t *select = (volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_SELECT);
    volatile uint32_t *window = (volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_WINDOW);

    *select = index;
    *window = value;
}

/*
 * The acknowledgement itself. This register is not selected through the index
 * pair, so the write is a single indivisible access rather than a transaction;
 * volatile is here because it is the acknowledgement, and an acknowledgement
 * the compiler dropped or duplicated would either wedge the pin or clear a
 * remote IRR belonging to a delivery that has not happened yet.
 */
static void ioapic_write_eoi(const struct ioapic_unit *unit, uint8_t vector)
{
    volatile uint32_t *eoi = (volatile uint32_t *)(uintptr_t)
        (unit->address + IOAPIC_REGISTER_EOI);

    *eoi = vector;
}

static uint32_t redirection_index(uint32_t entry, bool high)
{
    return IOAPIC_INDEX_REDIRECTION + entry * 2U + (high ? 1U : 0U);
}

/* Compose the two halves of a redirection entry from what Phipia intends. */
static uint32_t entry_low(const struct ioapic_redirection *entry)
{
    uint32_t low = IOAPIC_DELIVERY_FIXED | IOAPIC_DESTINATION_PHYSICAL |
        entry->vector;

    if (entry->active_low) {
        low |= IOAPIC_POLARITY_ACTIVE_LOW;
    }

    if (entry->level_triggered) {
        low |= IOAPIC_TRIGGER_LEVEL;
    }

    if (entry->masked) {
        low |= IOAPIC_MASKED;
    }

    return low;
}

static uint32_t entry_high(const struct ioapic_redirection *entry)
{
    return (uint32_t)entry->destination << IOAPIC_DESTINATION_SHIFT;
}

/* And take one apart again, including the two bits only the device writes. */
static void entry_decompose(
    uint32_t low,
    uint32_t high,
    struct ioapic_redirection *entry
)
{
    entry->vector = (uint8_t)(low & IOAPIC_VECTOR_MASK);
    entry->destination = (uint8_t)
        ((high >> IOAPIC_DESTINATION_SHIFT) & IOAPIC_VECTOR_MASK);
    entry->active_low = (low & IOAPIC_POLARITY_ACTIVE_LOW) != 0U;
    entry->level_triggered = (low & IOAPIC_TRIGGER_LEVEL) != 0U;
    entry->masked = (low & IOAPIC_MASKED) != 0U;
    entry->remote_irr = (low & IOAPIC_REMOTE_IRR) != 0U;
}

/*
 * Intel SDM volume 3A section 11.5.5: the directed end-of-interrupt register
 * arrived with I/O APIC version 0x20. Older units have no way to clear a remote
 * IRR from software except the mask and unmask dance. Phipia refuses that
 * fallback because it cannot preserve the same explicit acknowledgement model.
 */
static bool version_has_directed_eoi(uint8_t version)
{
    return version >= IOAPIC_VERSION_DIRECTED_EOI;
}

static bool can_select_directed_eoi(
    bool local_apic_suppression_supported,
    bool ioapic_directed_eoi_supported
)
{
    return local_apic_suppression_supported && ioapic_directed_eoi_supported;
}

static void mask_entry(const struct ioapic_unit *unit, uint32_t entry)
{
    /*
     * Mask before touching the destination so no interrupt can be delivered
     * against a half-written entry.
     */
    ioapic_write(unit, redirection_index(entry, false), IOAPIC_MASKED);
    ioapic_write(unit, redirection_index(entry, true), 0U);
}

/*
 * Translate an ISA IRQ into the global system interrupt and electrical
 * behaviour the firmware declared for it. Without an override, ACPI 6.6 section
 * 5.2.12.5 says an ISA IRQ is identity mapped, edge triggered, and active high.
 */
static enum ioapic_status resolve_isa_irq(
    const struct acpi_topology *topology,
    uint8_t irq,
    struct ioapic_source *source
)
{
    source->global_interrupt = irq;
    source->active_low = false;
    source->level_triggered = false;

    if (topology == NULL) {
        acpi_bytes_zero(source, sizeof(*source));
        return IOAPIC_STATUS_NULL_ARGUMENT;
    }

    for (size_t index = 0; index < topology->interrupt_override_count; ++index) {
        const struct acpi_interrupt_override *override =
            &topology->interrupt_overrides[index];
        uint16_t polarity;
        uint16_t trigger;

        if (override->source != irq) {
            continue;
        }

        polarity = override->flags & MPS_INTI_POLARITY_MASK;
        trigger = override->flags & MPS_INTI_TRIGGER_MASK;

        if (polarity == MPS_INTI_POLARITY_RESERVED) {
            acpi_bytes_zero(source, sizeof(*source));
            return IOAPIC_STATUS_RESERVED_POLARITY;
        }

        if (trigger == MPS_INTI_TRIGGER_RESERVED) {
            acpi_bytes_zero(source, sizeof(*source));
            return IOAPIC_STATUS_RESERVED_TRIGGER;
        }

        source->global_interrupt = override->global_system_interrupt;
        source->active_low = polarity == MPS_INTI_POLARITY_ACTIVE_LOW;
        source->level_triggered = trigger == MPS_INTI_TRIGGER_LEVEL;
        return IOAPIC_STATUS_OK;
    }

    return IOAPIC_STATUS_OK;
}

static enum ioapic_status validate_route_request(
    uint8_t irq,
    uint8_t vector,
    uint32_t destination,
    enum ioapic_trigger trigger
)
{
    if (irq >= IOAPIC_ISA_IRQ_COUNT) {
        return IOAPIC_STATUS_BAD_IRQ;
    }

    if (vector < INTERRUPT_IOAPIC_BASE || vector >= INTERRUPT_IOAPIC_LIMIT) {
        return IOAPIC_STATUS_BAD_VECTOR;
    }

    /*
     * The destination field is eight bits wide, and physical mode puts the
     * target APIC identifier in it. A wider identifier would shift into the
     * reserved half of the upper word instead of being clamped.
     */
    if (destination > UINT8_MAX) {
        return IOAPIC_STATUS_BAD_DESTINATION;
    }

    if (trigger != IOAPIC_TRIGGER_FIRMWARE &&
        trigger != IOAPIC_TRIGGER_FORCE_LEVEL) {
        return IOAPIC_STATUS_BAD_TRIGGER;
    }

    return IOAPIC_STATUS_OK;
}

static struct ioapic_unit *unit_for_interrupt(
    uint32_t global_interrupt,
    uint32_t *entry
)
{
    for (size_t index = 0; index < state.count; ++index) {
        struct ioapic_unit *unit = &state.units[index];

        if (global_interrupt >= unit->interrupt_base &&
            global_interrupt - unit->interrupt_base < unit->entry_count) {
            *entry = global_interrupt - unit->interrupt_base;
            return unit;
        }
    }

    return NULL;
}

static struct ioapic_route *route_for_vector(uint8_t vector)
{
    if (vector < INTERRUPT_IOAPIC_BASE || vector >= INTERRUPT_IOAPIC_LIMIT) {
        return NULL;
    }

    return &routes[vector - INTERRUPT_IOAPIC_BASE];
}

static void forget_route(struct ioapic_route *route)
{
    if (route->routed && route->level_triggered && state.level_routes != 0U) {
        --state.level_routes;
    }

    route->routed = false;
    route->level_triggered = false;
    route->unit = 0U;
    route->entry = 0U;
}

/*
 * Forget every vector that currently names this pin. Re-routing a pin to a new
 * vector would otherwise leave the old record behind, and a directed end of
 * interrupt sent for that stale vector would reach a live entry the kernel no
 * longer believes it owns.
 */
static void forget_entry(uint8_t unit_index, uint32_t entry)
{
    for (size_t index = 0; index < IOAPIC_ROUTABLE_VECTORS; ++index) {
        struct ioapic_route *route = &routes[index];

        if (route->routed && route->unit == unit_index &&
            route->entry == entry) {
            forget_route(route);
        }
    }
}

/* The routed topology is kept so masking can find the entry again. */
static const struct acpi_topology *routing_topology;
static bool level_eoi_mode_selected;

enum ioapic_status ioapic_initialize(const struct acpi_topology *topology)
{
    if (topology == NULL) {
        return IOAPIC_STATUS_NULL_ARGUMENT;
    }

    if (initialized) {
        return IOAPIC_STATUS_ALREADY_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return IOAPIC_STATUS_INTERRUPTS_ENABLED;
    }

    acpi_bytes_zero(&state, sizeof(state));
    acpi_bytes_zero(routes, sizeof(routes));

    if (topology->io_apic_count == 0U) {
        return IOAPIC_STATUS_MISSING_IO_APIC;
    }

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        const struct acpi_io_apic *described = &topology->io_apics[index];
        struct ioapic_unit *unit = &state.units[index];
        uint32_t version;

        unit->identifier = described->identifier;
        unit->address = described->address;
        unit->interrupt_base = described->interrupt_base;
        version = ioapic_read(unit, IOAPIC_INDEX_VERSION);
        unit->version = (uint8_t)(version & UINT32_C(0xFF));
        unit->entry_count = (uint8_t)(((version >> 16U) & UINT32_C(0xFF)) + 1U);
        unit->directed_eoi = version_has_directed_eoi(unit->version);

        if ((ioapic_read(unit, IOAPIC_INDEX_ID) >> IOAPIC_DESTINATION_SHIFT &
             UINT32_C(0x0F)) != described->identifier) {
            acpi_bytes_zero(&state, sizeof(state));
            return IOAPIC_STATUS_ID_DISAGREES_WITH_ACPI;
        }

        if (unit->entry_count < IOAPIC_MIN_ENTRIES) {
            acpi_bytes_zero(&state, sizeof(state));
            return IOAPIC_STATUS_TOO_FEW_ENTRIES;
        }

        /*
         * Two I/O APICs claiming the same global interrupt would make routing
         * ambiguous, and the first match would silently win.
         */
        for (size_t earlier = 0; earlier < index; ++earlier) {
            const struct ioapic_unit *other = &state.units[earlier];

            if (unit->interrupt_base <
                    other->interrupt_base + other->entry_count &&
                other->interrupt_base <
                    unit->interrupt_base + unit->entry_count) {
                acpi_bytes_zero(&state, sizeof(state));
                return IOAPIC_STATUS_OVERLAPPING_INTERRUPT_BASE;
            }
        }

        state.count = index + 1U;

        /* Firmware may leave any entry unmasked; none of them are Phipia's. */
        for (uint32_t entry = 0; entry < unit->entry_count; ++entry) {
            mask_entry(unit, entry);
        }
    }

    routing_topology = topology;
    initialized = true;
    return IOAPIC_STATUS_OK;
}

enum ioapic_status ioapic_route_isa_irq_as(
    uint8_t irq,
    uint8_t vector,
    uint32_t destination,
    enum ioapic_trigger trigger
)
{
    struct ioapic_redirection intent;
    struct ioapic_redirection observed;
    struct ioapic_source source;
    struct ioapic_route *route;
    struct ioapic_unit *unit;
    uint32_t entry = 0U;
    enum ioapic_status status;

    if (!initialized) {
        return IOAPIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return IOAPIC_STATUS_INTERRUPTS_ENABLED;
    }

    status = validate_route_request(irq, vector, destination, trigger);

    if (status != IOAPIC_STATUS_OK) {
        return status;
    }

    status = resolve_isa_irq(routing_topology, irq, &source);

    if (status != IOAPIC_STATUS_OK) {
        return status;
    }

    if (trigger == IOAPIC_TRIGGER_FORCE_LEVEL) {
        source.level_triggered = true;
    }

    unit = unit_for_interrupt(source.global_interrupt, &entry);

    if (unit == NULL) {
        return IOAPIC_STATUS_UNROUTABLE_INTERRUPT;
    }

    if (source.level_triggered && !level_eoi_mode_selected) {
        const struct apic_state apic = apic_get_state();

        /*
         * Directed EOI is selected only when both ends advertise it. Otherwise
         * the architected local-APIC broadcast remains the acknowledgement.
         * The choice is global and permanent because SVR bit 12 is global too.
         */
        if (can_select_directed_eoi(
                apic.eoi_broadcast_suppression_supported,
                unit->directed_eoi
            )) {
            if (apic_suppress_eoi_broadcasts() != APIC_STATUS_OK) {
                return IOAPIC_STATUS_LOCAL_EOI_SUPPRESSION_FAILURE;
            }

            state.directed_eoi_mode = true;
        }

        level_eoi_mode_selected = true;
    }

    if (source.level_triggered && state.directed_eoi_mode &&
        !unit->directed_eoi) {
        return IOAPIC_STATUS_NO_DIRECTED_EOI;
    }

    route = route_for_vector(vector);

    /*
     * A vector may name only one pin at a time. Re-pointing it would leave the
     * pin it named before unmasked and delivering a vector the dispatcher would
     * then acknowledge somewhere else, so the caller has to mask that entry
     * first rather than have this quietly do it.
     */
    if (route->routed &&
        (route->unit != (uint8_t)(unit - state.units) ||
         route->entry != (uint8_t)entry)) {
        return IOAPIC_STATUS_VECTOR_IN_USE;
    }

    acpi_bytes_zero(&intent, sizeof(intent));
    intent.vector = vector;
    intent.destination = (uint8_t)destination;
    intent.active_low = source.active_low;
    intent.level_triggered = source.level_triggered;
    intent.masked = false;
    intent.remote_irr = false;

    /* Destination first, unmasked vector last, so no partial entry can fire. */
    mask_entry(unit, entry);
    forget_route(route);
    forget_entry((uint8_t)(unit - state.units), entry);
    ioapic_write(unit, redirection_index(entry, true), entry_high(&intent));
    ioapic_write(unit, redirection_index(entry, false), entry_low(&intent));

    /*
     * Read the entry back and compare only the fields Phipia wrote. Delivery
     * status and remote IRR are the device's, and either can be set the instant
     * an unmasked pin asserts, so comparing the raw word would make a correctly
     * programmed level-triggered entry fail whenever its line was already busy.
     */
    entry_decompose(
        ioapic_read(unit, redirection_index(entry, false)),
        ioapic_read(unit, redirection_index(entry, true)),
        &observed
    );

    if (observed.vector != intent.vector ||
        observed.destination != intent.destination ||
        observed.active_low != intent.active_low ||
        observed.level_triggered != intent.level_triggered ||
        observed.masked) {
        mask_entry(unit, entry);
        return IOAPIC_STATUS_READBACK_MISMATCH;
    }

    route->routed = true;
    route->level_triggered = intent.level_triggered;
    route->unit = (uint8_t)(unit - state.units);
    route->entry = (uint8_t)entry;

    if (route->level_triggered) {
        ++state.level_routes;
    }

    return IOAPIC_STATUS_OK;
}

enum ioapic_status ioapic_route_isa_irq(
    uint8_t irq,
    uint8_t vector,
    uint32_t destination
)
{
    return ioapic_route_isa_irq_as(
        irq,
        vector,
        destination,
        IOAPIC_TRIGGER_FIRMWARE
    );
}

enum ioapic_status ioapic_mask_isa_irq(uint8_t irq)
{
    struct ioapic_source source;
    struct ioapic_unit *unit;
    uint32_t entry = 0U;
    enum ioapic_status status;

    if (!initialized) {
        return IOAPIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return IOAPIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (irq >= IOAPIC_ISA_IRQ_COUNT) {
        return IOAPIC_STATUS_BAD_IRQ;
    }

    status = resolve_isa_irq(routing_topology, irq, &source);

    if (status != IOAPIC_STATUS_OK) {
        return status;
    }

    unit = unit_for_interrupt(source.global_interrupt, &entry);

    if (unit == NULL) {
        return IOAPIC_STATUS_UNROUTABLE_INTERRUPT;
    }

    mask_entry(unit, entry);
    forget_entry((uint8_t)(unit - state.units), entry);
    return IOAPIC_STATUS_OK;
}

bool ioapic_vector_is_level_triggered(uint8_t vector)
{
    const struct ioapic_route *route = route_for_vector(vector);

    return initialized && route != NULL && route->routed &&
        route->level_triggered;
}

enum ioapic_status ioapic_send_eoi(uint8_t vector)
{
    struct ioapic_redirection observed;
    const struct ioapic_route *route;
    const struct ioapic_unit *unit;

    if (!initialized) {
        return IOAPIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return IOAPIC_STATUS_INTERRUPTS_ENABLED;
    }

    route = route_for_vector(vector);

    if (route == NULL) {
        return IOAPIC_STATUS_BAD_VECTOR;
    }

    if (!route->routed) {
        return IOAPIC_STATUS_VECTOR_NOT_ROUTED;
    }

    if (!route->level_triggered) {
        return IOAPIC_STATUS_NOT_LEVEL_TRIGGERED;
    }

    unit = &state.units[route->unit];
    entry_decompose(
        ioapic_read(unit, redirection_index(route->entry, false)),
        0U,
        &observed
    );

    /*
     * A level-triggered delivery latches remote IRR, so it is set here on every
     * interrupt this path acknowledges. Counting both answers is what makes an
     * entry that is level triggered in Phipia's records but edge triggered in
     * the hardware observable: it delivers perfectly well and never latches
     * anything, so nothing else would notice.
     */
    if (observed.remote_irr) {
        ++state.remote_irr_observed;
    } else {
        ++state.remote_irr_missing;
    }

    /*
     * Intel SDM volume 3A section 13.8.5 orders the directed protocol: local
     * APIC EOI first, then the generating I/O APIC's directed EOI. When local
     * broadcast suppression is unavailable, that first write is itself what
     * clears remote IRR and no redundant directed write is sent.
     */
    apic_send_eoi();

    if (state.directed_eoi_mode) {
        ioapic_write_eoi(unit, vector);
        ++state.directed_eoi_count;
    }

    /*
     * Do not read the entry back here. A still-asserted line may already have
     * latched remote IRR for its next delivery, which is indistinguishable from
     * a failed acknowledgement.
     */
    return IOAPIC_STATUS_OK;
}

enum ioapic_status ioapic_read_redirection(
    uint8_t vector,
    struct ioapic_redirection *entry
)
{
    const struct ioapic_route *route;
    const struct ioapic_unit *unit;

    if (entry == NULL) {
        return IOAPIC_STATUS_NULL_ARGUMENT;
    }

    acpi_bytes_zero(entry, sizeof(*entry));

    if (!initialized) {
        return IOAPIC_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return IOAPIC_STATUS_INTERRUPTS_ENABLED;
    }

    route = route_for_vector(vector);

    if (route == NULL) {
        return IOAPIC_STATUS_BAD_VECTOR;
    }

    if (!route->routed) {
        return IOAPIC_STATUS_VECTOR_NOT_ROUTED;
    }

    unit = &state.units[route->unit];
    entry_decompose(
        ioapic_read(unit, redirection_index(route->entry, false)),
        ioapic_read(unit, redirection_index(route->entry, true)),
        entry
    );
    entry->unit_identifier = unit->identifier;
    entry->global_interrupt = unit->interrupt_base + route->entry;
    return IOAPIC_STATUS_OK;
}

struct ioapic_state ioapic_get_state(void)
{
    return state;
}

bool ioapic_is_initialized(void)
{
    return initialized;
}

/*
 * A redirection entry is composed from an intent and taken apart again by the
 * dispatcher's acknowledgement path, and neither half touches hardware. A bit
 * placed wrongly in either direction is the failure that would program a
 * correct-looking entry with the wrong electricals, so both directions are
 * driven here from synthetic register values.
 */
static bool entry_round_trips(bool active_low, bool level_triggered, bool masked)
{
    struct ioapic_redirection intent;
    struct ioapic_redirection observed;
    uint32_t low;
    uint32_t high;

    acpi_bytes_zero(&intent, sizeof(intent));
    intent.vector = INTERRUPT_IOAPIC_BASE + 3U;
    intent.destination = UINT8_C(0xA5);
    intent.active_low = active_low;
    intent.level_triggered = level_triggered;
    intent.masked = masked;
    intent.remote_irr = false;

    low = entry_low(&intent);
    high = entry_high(&intent);

    /* Composition must never place a bit the I/O APIC owns. */
    if ((low & ~IOAPIC_ENTRY_WRITABLE_MASK) != 0U ||
        (low & IOAPIC_DESTINATION_LOGICAL) != 0U ||
        (low & IOAPIC_DELIVERY_MODE_MASK) != 0U) {
        return false;
    }

    /* And the exact bits the datasheet assigns, not merely self-consistent. */
    if (((low & IOAPIC_POLARITY_ACTIVE_LOW) != 0U) != active_low ||
        ((low & IOAPIC_TRIGGER_LEVEL) != 0U) != level_triggered ||
        ((low & IOAPIC_MASKED) != 0U) != masked ||
        (low & IOAPIC_VECTOR_MASK) != intent.vector ||
        high != ((uint32_t)intent.destination << IOAPIC_DESTINATION_SHIFT)) {
        return false;
    }

    entry_decompose(low, high, &observed);

    if (observed.vector != intent.vector ||
        observed.destination != intent.destination ||
        observed.active_low != active_low ||
        observed.level_triggered != level_triggered ||
        observed.masked != masked || observed.remote_irr) {
        return false;
    }

    /* Remote IRR is the device's to set, and must decode when it does. */
    entry_decompose(low | IOAPIC_REMOTE_IRR, high, &observed);
    return observed.remote_irr && observed.level_triggered == level_triggered;
}

static bool entries_round_trip(void)
{
    return entry_round_trips(false, false, false) &&
        entry_round_trips(false, true, false) &&
        entry_round_trips(true, false, false) &&
        entry_round_trips(true, true, false) &&
        entry_round_trips(false, true, true) &&
        entry_round_trips(true, false, true);
}

/*
 * The version check that decides whether a level-triggered entry may exist at
 * all. Assuming the register is there would program a pin that latches remote
 * IRR with nothing able to clear it.
 */
static bool directed_eoi_is_gated_on_version(void)
{
    return !version_has_directed_eoi(UINT8_C(0x00)) &&
        !version_has_directed_eoi(UINT8_C(0x11)) &&
        !version_has_directed_eoi(UINT8_C(0x1F)) &&
        version_has_directed_eoi(IOAPIC_VERSION_DIRECTED_EOI) &&
        version_has_directed_eoi(UINT8_C(0x21)) &&
        version_has_directed_eoi(UINT8_C(0xFF)) &&
        !can_select_directed_eoi(false, false) &&
        !can_select_directed_eoi(false, true) &&
        !can_select_directed_eoi(true, false) &&
        can_select_directed_eoi(true, true);
}

static bool source_matches(
    const struct acpi_topology *topology,
    uint8_t irq,
    uint32_t global_interrupt,
    bool active_low,
    bool level_triggered
)
{
    struct ioapic_source source;

    if (resolve_isa_irq(topology, irq, &source) != IOAPIC_STATUS_OK) {
        return false;
    }

    return source.global_interrupt == global_interrupt &&
        source.active_low == active_low &&
        source.level_triggered == level_triggered;
}

static bool source_is_rejected(
    const struct acpi_topology *topology,
    uint8_t irq,
    enum ioapic_status expected
)
{
    struct ioapic_source source;

    source.global_interrupt = UINT32_C(0xDEADBEEF);
    source.active_low = true;
    source.level_triggered = true;

    if (resolve_isa_irq(topology, irq, &source) != expected) {
        return false;
    }

    /* Every rejection leaves the output zeroed rather than half filled. */
    return source.global_interrupt == 0U && !source.active_low &&
        !source.level_triggered;
}

static bool overrides_are_resolved(void)
{
    struct acpi_topology topology;

    acpi_bytes_zero(&topology, sizeof(topology));

    /* No override: identity mapped, edge triggered, active high. */
    if (!source_matches(&topology, 7U, 7U, false, false)) {
        return false;
    }

    /* The customary IRQ0 to GSI2 override with conforming ISA electricals. */
    topology.interrupt_override_count = 4U;
    topology.interrupt_overrides[0].source = 0U;
    topology.interrupt_overrides[0].global_system_interrupt = 2U;
    topology.interrupt_overrides[0].flags = MPS_INTI_POLARITY_CONFORMING |
        MPS_INTI_TRIGGER_CONFORMING;
    topology.interrupt_overrides[1].source = 9U;
    topology.interrupt_overrides[1].global_system_interrupt = 9U;
    topology.interrupt_overrides[1].flags = MPS_INTI_POLARITY_ACTIVE_HIGH |
        MPS_INTI_TRIGGER_LEVEL;
    topology.interrupt_overrides[2].source = 10U;
    topology.interrupt_overrides[2].global_system_interrupt = 10U;
    topology.interrupt_overrides[2].flags = MPS_INTI_POLARITY_ACTIVE_LOW |
        MPS_INTI_TRIGGER_LEVEL;
    topology.interrupt_overrides[3].source = 11U;
    topology.interrupt_overrides[3].global_system_interrupt = 11U;
    topology.interrupt_overrides[3].flags = MPS_INTI_POLARITY_ACTIVE_LOW |
        MPS_INTI_TRIGGER_EDGE;

    /* All four corners of the polarity and trigger encoding. */
    return source_matches(&topology, 0U, 2U, false, false) &&
        source_matches(&topology, 9U, 9U, false, true) &&
        source_matches(&topology, 10U, 10U, true, true) &&
        source_matches(&topology, 11U, 11U, true, false) &&
        /* An unoverridden IRQ keeps its mapping beside overridden ones. */
        source_matches(&topology, 4U, 4U, false, false);
}

static bool reserved_encodings_are_refused(void)
{
    struct acpi_topology topology;

    acpi_bytes_zero(&topology, sizeof(topology));
    topology.interrupt_override_count = 2U;
    topology.interrupt_overrides[0].source = 5U;
    topology.interrupt_overrides[0].global_system_interrupt = 5U;
    topology.interrupt_overrides[0].flags = MPS_INTI_POLARITY_RESERVED |
        MPS_INTI_TRIGGER_LEVEL;
    topology.interrupt_overrides[1].source = 6U;
    topology.interrupt_overrides[1].global_system_interrupt = 6U;
    topology.interrupt_overrides[1].flags = MPS_INTI_POLARITY_ACTIVE_HIGH |
        MPS_INTI_TRIGGER_RESERVED;

    return source_is_rejected(&topology, 5U, IOAPIC_STATUS_RESERVED_POLARITY) &&
        source_is_rejected(&topology, 6U, IOAPIC_STATUS_RESERVED_TRIGGER) &&
        source_is_rejected(NULL, 0U, IOAPIC_STATUS_NULL_ARGUMENT);
}

static bool route_requests_are_validated(void)
{
    const uint8_t vector = INTERRUPT_IOAPIC_BASE;

    return validate_route_request(0U, vector, 0U, IOAPIC_TRIGGER_FIRMWARE) ==
            IOAPIC_STATUS_OK &&
        validate_route_request(IOAPIC_ISA_IRQ_COUNT - 1U, vector, UINT8_MAX,
            IOAPIC_TRIGGER_FORCE_LEVEL) == IOAPIC_STATUS_OK &&
        validate_route_request(IOAPIC_ISA_IRQ_COUNT, vector, 0U,
            IOAPIC_TRIGGER_FIRMWARE) == IOAPIC_STATUS_BAD_IRQ &&
        validate_route_request(0U, INTERRUPT_IOAPIC_BASE - 1U, 0U,
            IOAPIC_TRIGGER_FIRMWARE) == IOAPIC_STATUS_BAD_VECTOR &&
        validate_route_request(0U, INTERRUPT_IOAPIC_LIMIT, 0U,
            IOAPIC_TRIGGER_FIRMWARE) == IOAPIC_STATUS_BAD_VECTOR &&
        validate_route_request(0U, vector, UINT8_MAX + 1U,
            IOAPIC_TRIGGER_FIRMWARE) == IOAPIC_STATUS_BAD_DESTINATION &&
        validate_route_request(0U, vector, 0U, (enum ioapic_trigger)2) ==
            IOAPIC_STATUS_BAD_TRIGGER;
}

/*
 * The public refusals that do not need hardware. This runs during early boot,
 * before ioapic_initialize, so it also pins down what the subsystem does before
 * it owns anything: every mutation and every read is refused by name.
 */
static bool public_entry_points_refuse_uninitialized(void)
{
    struct ioapic_redirection entry;

    entry.unit_identifier = UINT32_C(0xFEEDFACE);
    entry.global_interrupt = UINT32_C(0xDEADBEEF);
    entry.vector = UINT8_C(0xEE);
    entry.destination = UINT8_C(0xEE);
    entry.active_low = true;
    entry.level_triggered = true;
    entry.masked = true;
    entry.remote_irr = true;

    /* Every rejection leaves the caller's entry zeroed, not half filled. */
    if (ioapic_read_redirection(INTERRUPT_IOAPIC_BASE, &entry) !=
            IOAPIC_STATUS_NOT_INITIALIZED ||
        entry.unit_identifier != 0U || entry.global_interrupt != 0U ||
        entry.vector != 0U || entry.destination != 0U || entry.active_low ||
        entry.level_triggered || entry.masked || entry.remote_irr) {
        return false;
    }

    return ioapic_initialize(NULL) == IOAPIC_STATUS_NULL_ARGUMENT &&
        ioapic_read_redirection(INTERRUPT_IOAPIC_BASE, NULL) ==
            IOAPIC_STATUS_NULL_ARGUMENT &&
        ioapic_route_isa_irq(0U, INTERRUPT_IOAPIC_BASE, 0U) ==
            IOAPIC_STATUS_NOT_INITIALIZED &&
        ioapic_route_isa_irq_as(0U, INTERRUPT_IOAPIC_BASE, 0U,
            IOAPIC_TRIGGER_FORCE_LEVEL) == IOAPIC_STATUS_NOT_INITIALIZED &&
        ioapic_mask_isa_irq(0U) == IOAPIC_STATUS_NOT_INITIALIZED &&
        ioapic_send_eoi(INTERRUPT_IOAPIC_BASE) ==
            IOAPIC_STATUS_NOT_INITIALIZED &&
        !ioapic_vector_is_level_triggered(INTERRUPT_IOAPIC_BASE);
}

/*
 * The route table decides which unit a directed end of interrupt reaches, and
 * it is the one piece of the acknowledgement path that can be driven without an
 * interrupt. A vector outside the delivery window, a vector inside it that
 * nothing routed, and an edge-triggered vector are three different answers, and
 * confusing any two would either acknowledge the wrong pin or not acknowledge
 * at all.
 */
static bool acknowledgement_targets_are_resolved(void)
{
    struct ioapic_route *route = route_for_vector(INTERRUPT_IOAPIC_BASE);

    if (route_for_vector(INTERRUPT_IOAPIC_BASE - 1U) != NULL ||
        route_for_vector(INTERRUPT_IOAPIC_LIMIT) != NULL ||
        route_for_vector(INTERRUPT_LOCAL_APIC_BASE) != NULL ||
        route != &routes[0] ||
        route_for_vector(INTERRUPT_IOAPIC_LIMIT - 1U) !=
            &routes[IOAPIC_ROUTABLE_VECTORS - 1U]) {
        return false;
    }

    /* A record is only forgotten once, so the level count cannot run away. */
    route->routed = true;
    route->level_triggered = true;
    route->unit = 0U;
    route->entry = 2U;
    state.level_routes = 1U;
    forget_entry(0U, 3U);

    if (!route->routed || state.level_routes != 1U) {
        return false;
    }

    forget_entry(0U, 2U);
    forget_route(route);
    return !route->routed && !route->level_triggered &&
        state.level_routes == 0U;
}

bool ioapic_self_test(void)
{
    return overrides_are_resolved() &&
        reserved_encodings_are_refused() &&
        entries_round_trip() &&
        directed_eoi_is_gated_on_version() &&
        route_requests_are_validated() &&
        acknowledgement_targets_are_resolved() &&
        public_entry_points_refuse_uninitialized();
}

const char *ioapic_status_string(enum ioapic_status status)
{
    static const char *const messages[IOAPIC_STATUS_COUNT] = {
        [IOAPIC_STATUS_OK] = "ok",
        [IOAPIC_STATUS_NULL_ARGUMENT] = "null I/O APIC argument",
        [IOAPIC_STATUS_ALREADY_INITIALIZED] =
            "I/O APIC was initialized twice",
        [IOAPIC_STATUS_NOT_INITIALIZED] = "I/O APIC is not initialized",
        [IOAPIC_STATUS_INTERRUPTS_ENABLED] =
            "I/O APIC mutation requires interrupts disabled",
        [IOAPIC_STATUS_MISSING_IO_APIC] =
            "ACPI described no I/O APIC to program",
        [IOAPIC_STATUS_ID_DISAGREES_WITH_ACPI] =
            "I/O APIC identifier disagrees with the ACPI MADT",
        [IOAPIC_STATUS_TOO_FEW_ENTRIES] =
            "I/O APIC cannot redirect the sixteen ISA interrupts",
        [IOAPIC_STATUS_OVERLAPPING_INTERRUPT_BASE] =
            "two I/O APICs claim the same global interrupt",
        [IOAPIC_STATUS_BAD_IRQ] = "interrupt request is outside the ISA range",
        [IOAPIC_STATUS_BAD_VECTOR] =
            "vector is outside the I/O APIC delivery range",
        [IOAPIC_STATUS_BAD_DESTINATION] =
            "destination does not fit the redirection entry",
        [IOAPIC_STATUS_BAD_TRIGGER] =
            "unknown I/O APIC trigger mode request",
        [IOAPIC_STATUS_RESERVED_POLARITY] =
            "ACPI declared a reserved interrupt polarity",
        [IOAPIC_STATUS_RESERVED_TRIGGER] =
            "ACPI declared a reserved interrupt trigger mode",
        [IOAPIC_STATUS_UNROUTABLE_INTERRUPT] =
            "no I/O APIC owns that global system interrupt",
        [IOAPIC_STATUS_NO_DIRECTED_EOI] =
            "I/O APIC predates the directed end-of-interrupt register",
        [IOAPIC_STATUS_VECTOR_IN_USE] =
            "that vector already carries a different redirection entry",
        [IOAPIC_STATUS_VECTOR_NOT_ROUTED] =
            "no redirection entry carries that vector",
        [IOAPIC_STATUS_NOT_LEVEL_TRIGGERED] =
            "an edge-triggered vector has no remote IRR to clear",
        [IOAPIC_STATUS_READBACK_MISMATCH] =
            "I/O APIC did not read back its programmed entry",
        [IOAPIC_STATUS_LOCAL_EOI_SUPPRESSION_FAILURE] =
            "local APIC could not enable EOI-broadcast suppression"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) == IOAPIC_STATUS_COUNT,
        "I/O APIC status table must cover every enumerator"
    );

    if ((unsigned int)status >= IOAPIC_STATUS_COUNT ||
        messages[status] == NULL) {
        return "unknown I/O APIC status";
    }

    return messages[status];
}
