/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/acpi.h>
#include <phipia/acpi_util.h>
#include <phipia/apic.h>
#include <phipia/boot.h>
#include <phipia/cpu.h>
#include <phipia/interrupts.h>

/* Intel SDM volume 3A table 11-1 fixes these register offsets. */
#define APIC_REGISTER_ID UINT32_C(0x0020)
#define APIC_REGISTER_VERSION UINT32_C(0x0030)
#define APIC_REGISTER_EOI UINT32_C(0x00B0)
#define APIC_REGISTER_SPURIOUS UINT32_C(0x00F0)
#define APIC_REGISTER_ERROR_STATUS UINT32_C(0x0280)
#define APIC_REGISTER_LVT_BASE UINT32_C(0x0320)
#define APIC_REGISTER_WINDOW_SIZE UINT64_C(0x1000)

/*
 * Intel SDM volume 3A section 11.5.1 orders the local vector table entries at
 * sixteen-byte intervals from the timer entry: timer, thermal, performance
 * counter, LINT0, LINT1, error.
 */
#define APIC_LVT_STRIDE UINT32_C(0x10)
#define APIC_LVT_TIMER 0U
#define APIC_LVT_THERMAL 1U
#define APIC_LVT_PERFORMANCE 2U
#define APIC_LVT_LINT0 3U
#define APIC_LVT_LINT1 4U
#define APIC_LVT_ERROR 5U
#define APIC_LVT_REQUIRED_ENTRIES 5U

/* Intel SDM volume 3A figure 11-8 defines the local vector table layout. */
#define APIC_LVT_DELIVERY_FIXED UINT32_C(0x00000000)
#define APIC_LVT_DELIVERY_NMI UINT32_C(0x00000400)
#define APIC_LVT_DELIVERY_EXTINT UINT32_C(0x00000700)
#define APIC_LVT_MASKED UINT32_C(0x00010000)

/* Intel SDM volume 3A figure 11-23 defines the spurious vector register. */
#define APIC_SPURIOUS_SOFTWARE_ENABLE UINT32_C(0x00000100)
#define APIC_SPURIOUS_SUPPRESS_EOI_BROADCASTS UINT32_C(0x00001000)

/* Intel SDM volume 3A section 11.4.4 defines IA32_APIC_BASE. */
#define IA32_APIC_BASE_MSR UINT32_C(0x0000001B)
#define APIC_BASE_BOOTSTRAP UINT64_C(0x0000000000000100)
#define APIC_BASE_X2APIC_ENABLE UINT64_C(0x0000000000000400)
#define APIC_BASE_GLOBAL_ENABLE UINT64_C(0x0000000000000800)
#define APIC_BASE_ADDRESS_MASK UINT64_C(0x000FFFFFFFFFF000)

/* Intel SDM volume 3A section 11.4.1: CPUID.01H:EDX[9] reports an APIC. */
#define CPUID_FEATURE_LEAF UINT32_C(1)
#define CPUID_FEATURE_APIC UINT32_C(0x00000200)

/*
 * Intel SDM volume 3A section 11.4.8: an integrated local APIC reports version
 * 0x10 through 0x15. Lower values describe the discrete 82489DX, which cannot
 * appear on a processor that supports long mode.
 */
#define APIC_VERSION_INTEGRATED_MASK UINT32_C(0xF0)
#define APIC_VERSION_INTEGRATED UINT32_C(0x10)
#define APIC_VERSION_EOI_BROADCAST_SUPPRESSION UINT32_C(0x01000000)

struct apic_base_info {
    uint64_t address;
    bool bootstrap_processor;
};

static struct apic_state state;
static volatile uint64_t spurious_counter __attribute__((aligned(8)));

/*
 * The local APIC registers are memory-mapped device state, not memory: their
 * value changes outside this program's control and every access must reach the
 * device exactly once, in program order. That is what volatile expresses here.
 * The SDM requires naturally aligned 32-bit accesses to these registers.
 */
static uint32_t apic_read(uint32_t offset)
{
    const volatile uint32_t *reg = (const volatile uint32_t *)(uintptr_t)
        (state.base_address + offset);

    return *reg;
}

static void apic_write(uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)
        (state.base_address + offset);

    *reg = value;
}

static uint32_t apic_lvt_offset(uint32_t index)
{
    return APIC_REGISTER_LVT_BASE + index * APIC_LVT_STRIDE;
}

/*
 * Decode IA32_APIC_BASE and hold the firmware description to it. A disagreement
 * between the MSR and the MADT means one of the two is describing a machine
 * Phipia is not running on, and neither is safe to program from.
 */
static enum apic_status decode_base(
    uint64_t msr_value,
    uint64_t acpi_address,
    struct apic_base_info *info
)
{
    const uint64_t address = msr_value & APIC_BASE_ADDRESS_MASK;

    if ((msr_value & APIC_BASE_GLOBAL_ENABLE) == 0U) {
        return APIC_STATUS_HARDWARE_DISABLED;
    }

    if ((msr_value & APIC_BASE_X2APIC_ENABLE) != 0U) {
        return APIC_STATUS_X2APIC_MODE;
    }

    if ((msr_value & APIC_BASE_BOOTSTRAP) == 0U) {
        return APIC_STATUS_NOT_BOOTSTRAP;
    }

    if (address == 0U) {
        return APIC_STATUS_NULL_BASE;
    }

    if (!acpi_span_is_early_mapped(address, APIC_REGISTER_WINDOW_SIZE)) {
        return APIC_STATUS_BASE_OUTSIDE_EARLY_MAP;
    }

    if (address != acpi_address) {
        return APIC_STATUS_BASE_DISAGREES_WITH_ACPI;
    }

    info->address = address;
    info->bootstrap_processor = true;
    return APIC_STATUS_OK;
}

static enum apic_status decode_version(
    uint32_t version_register,
    uint8_t *version,
    uint8_t *max_lvt_entry,
    bool *eoi_broadcast_suppression_supported
)
{
    const uint32_t raw_version = version_register & UINT32_C(0xFF);
    const uint32_t entries = (version_register >> 16U) & UINT32_C(0xFF);

    if ((raw_version & APIC_VERSION_INTEGRATED_MASK) !=
        APIC_VERSION_INTEGRATED) {
        return APIC_STATUS_EXTERNAL_APIC;
    }

    if (entries < APIC_LVT_REQUIRED_ENTRIES) {
        return APIC_STATUS_TOO_FEW_LVT_ENTRIES;
    }

    *version = (uint8_t)raw_version;
    *max_lvt_entry = (uint8_t)entries;
    *eoi_broadcast_suppression_supported =
        (version_register & APIC_VERSION_EOI_BROADCAST_SUPPRESSION) != 0U;
    return APIC_STATUS_OK;
}

/*
 * The ACPI processor list is the authority on which APIC identifier belongs to
 * an enabled processor. The running processor must appear in it as enabled.
 */
static enum apic_status check_identifier(
    const struct acpi_topology *topology,
    uint32_t identifier
)
{
    for (size_t index = 0; index < topology->local_apic_count; ++index) {
        const struct acpi_local_apic *processor = &topology->local_apics[index];

        if (processor->apic_id == identifier && processor->enabled) {
            return APIC_STATUS_OK;
        }
    }

    return APIC_STATUS_ID_DISAGREES_WITH_ACPI;
}

static void spurious_handler(struct interrupt_frame *frame, void *context)
{
    (void)frame;
    (void)context;
    ++spurious_counter;
}

/*
 * Leave every local vector table entry Phipia does not use in a known masked
 * state rather than inheriting whatever the firmware left behind. A masked
 * entry still carries a legal vector so an accidental unmask cannot raise an
 * illegal-vector error.
 */
static void mask_unused_lvt_entries(void)
{
    static const uint32_t unused[] = {
        APIC_LVT_TIMER,
        APIC_LVT_THERMAL,
        APIC_LVT_PERFORMANCE,
        APIC_LVT_ERROR
    };

    const size_t count = sizeof(unused) / sizeof(unused[0]);

    for (size_t index = 0; index < count; ++index) {
        if (unused[index] > state.max_lvt_entry) {
            continue;
        }

        apic_write(
            apic_lvt_offset(unused[index]),
            APIC_LVT_MASKED | APIC_LVT_DELIVERY_FIXED | APIC_SPURIOUS_VECTOR
        );
    }
}

/*
 * Virtual wire mode. Enabling the local APIC takes the 8259 pair off the
 * processor's direct interrupt path, so LINT0 must carry its output as ExtINT
 * or every legacy interrupt, including the PIT proof, silently stops arriving.
 * LINT1 becomes the NMI pin. A machine without a legacy PIC gets LINT0 masked.
 */
static void route_legacy_interrupts(bool legacy_pic_present)
{
    apic_write(
        apic_lvt_offset(APIC_LVT_LINT0),
        legacy_pic_present
            ? APIC_LVT_DELIVERY_EXTINT
            : (APIC_LVT_MASKED | APIC_LVT_DELIVERY_FIXED | APIC_SPURIOUS_VECTOR)
    );
    apic_write(apic_lvt_offset(APIC_LVT_LINT1), APIC_LVT_DELIVERY_NMI);
    state.legacy_interrupts_routed = legacy_pic_present;
}

static enum apic_status verify_programming(bool legacy_pic_present)
{
    const uint32_t spurious = apic_read(APIC_REGISTER_SPURIOUS);
    const uint32_t lint0 = apic_read(apic_lvt_offset(APIC_LVT_LINT0));
    const uint32_t lint1 = apic_read(apic_lvt_offset(APIC_LVT_LINT1));
    const uint32_t expected_lint0 = legacy_pic_present
        ? APIC_LVT_DELIVERY_EXTINT
        : (APIC_LVT_MASKED | APIC_LVT_DELIVERY_FIXED | APIC_SPURIOUS_VECTOR);

    if ((spurious & APIC_SPURIOUS_SOFTWARE_ENABLE) == 0U ||
        (spurious & UINT32_C(0xFF)) != APIC_SPURIOUS_VECTOR) {
        return APIC_STATUS_READBACK_MISMATCH;
    }

    /*
     * Delivery status and remote IRR are read-only status bits the processor
     * owns, so only the fields Phipia wrote are compared.
     */
    if ((lint0 & UINT32_C(0x1F7FF)) != expected_lint0 ||
        (lint1 & UINT32_C(0x1F7FF)) != APIC_LVT_DELIVERY_NMI) {
        return APIC_STATUS_READBACK_MISMATCH;
    }

    if (apic_read(APIC_REGISTER_ID) != (state.id << 24U)) {
        return APIC_STATUS_READBACK_MISMATCH;
    }

    return APIC_STATUS_OK;
}

enum apic_status apic_bring_online(const struct acpi_topology *topology)
{
    struct cpuid_result cpuid_features;
    struct apic_base_info base;
    enum apic_status status;
    enum interrupt_status interrupt_status;

    if (topology == NULL) {
        return APIC_STATUS_NULL_ARGUMENT;
    }

    if (state.online) {
        return APIC_STATUS_ALREADY_ONLINE;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_STATUS_INTERRUPTS_ENABLED;
    }

    cpu_cpuid(CPUID_FEATURE_LEAF, 0U, &cpuid_features);

    if ((cpuid_features.edx & CPUID_FEATURE_APIC) == 0U) {
        return APIC_STATUS_UNSUPPORTED;
    }

    status = decode_base(
        cpu_read_msr(IA32_APIC_BASE_MSR),
        topology->local_apic_address,
        &base
    );

    if (status != APIC_STATUS_OK) {
        return status;
    }

    state.base_address = base.address;
    status = decode_version(
        apic_read(APIC_REGISTER_VERSION),
        &state.version,
        &state.max_lvt_entry,
        &state.eoi_broadcast_suppression_supported
    );

    if (status != APIC_STATUS_OK) {
        state.base_address = 0U;
        return status;
    }

    state.id = apic_read(APIC_REGISTER_ID) >> 24U;
    status = check_identifier(topology, state.id);

    if (status != APIC_STATUS_OK) {
        state.base_address = 0U;
        return status;
    }

    spurious_counter = 0U;
    interrupt_status = interrupt_register_handler(
        APIC_SPURIOUS_VECTOR,
        spurious_handler,
        NULL
    );

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        state.base_address = 0U;
        return APIC_STATUS_INTERRUPT_FAILURE;
    }

    /*
     * Software-enable first: the SDM forces every local vector table mask bit
     * while the APIC is software-disabled, so the entries below must be written
     * after the enable to take effect.
     */
    apic_write(
        APIC_REGISTER_SPURIOUS,
        APIC_SPURIOUS_SOFTWARE_ENABLE | APIC_SPURIOUS_VECTOR
    );
    mask_unused_lvt_entries();
    route_legacy_interrupts(topology->legacy_pic_present);
    apic_write(APIC_REGISTER_ERROR_STATUS, 0U);

    status = verify_programming(topology->legacy_pic_present);

    if (status != APIC_STATUS_OK) {
        (void)interrupt_unregister_handler(APIC_SPURIOUS_VECTOR);
        state.base_address = 0U;
        return status;
    }

    state.online = true;
    return APIC_STATUS_OK;
}

/*
 * Intel SDM volume 3A section 13.8.5 permits directed EOI only after the local
 * APIC advertises the feature in version bit 24 and SVR bit 12 reads back set.
 * The caller keeps interrupts disabled so no level interrupt can be completed
 * while the machine is between broadcast and directed acknowledgement modes.
 */
enum apic_status apic_suppress_eoi_broadcasts(void)
{
    uint32_t spurious;

    if (!state.online) {
        return APIC_STATUS_NOT_ONLINE;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_STATUS_INTERRUPTS_ENABLED;
    }

    if (!state.eoi_broadcast_suppression_supported) {
        return APIC_STATUS_EOI_BROADCAST_SUPPRESSION_UNSUPPORTED;
    }

    if (state.eoi_broadcasts_suppressed) {
        return APIC_STATUS_OK;
    }

    spurious = apic_read(APIC_REGISTER_SPURIOUS);
    apic_write(
        APIC_REGISTER_SPURIOUS,
        spurious | APIC_SPURIOUS_SUPPRESS_EOI_BROADCASTS
    );

    if ((apic_read(APIC_REGISTER_SPURIOUS) &
         APIC_SPURIOUS_SUPPRESS_EOI_BROADCASTS) == 0U) {
        return APIC_STATUS_READBACK_MISMATCH;
    }

    state.eoi_broadcasts_suppressed = true;
    return APIC_STATUS_OK;
}

/*
 * Virtual wire mode exists only to carry the 8259 pair's output. Once the pair
 * is retired, an ExtINT LINT0 is a path for interrupts Phipia has decided not
 * to accept, so it is masked like every other unused entry.
 */
enum apic_status apic_retire_legacy_routing(void)
{
    const uint32_t masked =
        APIC_LVT_MASKED | APIC_LVT_DELIVERY_FIXED | APIC_SPURIOUS_VECTOR;

    if (!state.online) {
        return APIC_STATUS_NOT_ONLINE;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_STATUS_INTERRUPTS_ENABLED;
    }

    apic_write(apic_lvt_offset(APIC_LVT_LINT0), masked);

    if ((apic_read(apic_lvt_offset(APIC_LVT_LINT0)) & UINT32_C(0x1F7FF)) !=
        masked) {
        return APIC_STATUS_READBACK_MISMATCH;
    }

    state.legacy_interrupts_routed = false;
    return APIC_STATUS_OK;
}

/*
 * Intel SDM volume 3A section 11.8.5 requires a zero write to acknowledge an
 * APIC-delivered interrupt. An offline APIC has no register window, and no
 * vector it owns can have been delivered, so the write is simply skipped.
 */
void apic_send_eoi(void)
{
    if (!state.online) {
        return;
    }

    apic_write(APIC_REGISTER_EOI, 0U);
}

uint32_t apic_register_read(uint32_t offset)
{
    return state.online ? apic_read(offset) : 0U;
}

void apic_register_write(uint32_t offset, uint32_t value)
{
    if (state.online) {
        apic_write(offset, value);
    }
}

struct apic_state apic_get_state(void)
{
    return state;
}

bool apic_is_online(void)
{
    return state.online;
}

uint64_t apic_spurious_count(void)
{
    return spurious_counter;
}

/*
 * The decoders above are the part of bring-up that can be proved without
 * hardware, and they are the part that rejects a machine Phipia must not
 * program. Every rejection below is driven by a synthetic register value.
 */
bool apic_self_test(void)
{
    static const uint64_t acpi_address = UINT64_C(0xFEE00000);
    const uint64_t valid = acpi_address | APIC_BASE_GLOBAL_ENABLE |
        APIC_BASE_BOOTSTRAP;
    struct apic_base_info base = {0};
    uint8_t version = 0U;
    uint8_t entries = 0U;
    bool eoi_broadcast_suppression_supported = false;
    struct acpi_topology topology;

    if (decode_base(valid, acpi_address, &base) != APIC_STATUS_OK ||
        base.address != acpi_address ||
        !base.bootstrap_processor) {
        return false;
    }

    if (decode_base(
            valid & ~APIC_BASE_GLOBAL_ENABLE,
            acpi_address,
            &base
        ) != APIC_STATUS_HARDWARE_DISABLED) {
        return false;
    }

    if (decode_base(
            valid | APIC_BASE_X2APIC_ENABLE,
            acpi_address,
            &base
        ) != APIC_STATUS_X2APIC_MODE) {
        return false;
    }

    if (decode_base(
            valid & ~APIC_BASE_BOOTSTRAP,
            acpi_address,
            &base
        ) != APIC_STATUS_NOT_BOOTSTRAP) {
        return false;
    }

    if (decode_base(
            APIC_BASE_GLOBAL_ENABLE | APIC_BASE_BOOTSTRAP,
            acpi_address,
            &base
        ) != APIC_STATUS_NULL_BASE) {
        return false;
    }

    if (decode_base(
            PHIPIA_EARLY_PHYSICAL_LIMIT | APIC_BASE_GLOBAL_ENABLE |
                APIC_BASE_BOOTSTRAP,
            PHIPIA_EARLY_PHYSICAL_LIMIT,
            &base
        ) != APIC_STATUS_BASE_OUTSIDE_EARLY_MAP) {
        return false;
    }

    if (decode_base(valid, UINT64_C(0xFEE01000), &base) !=
        APIC_STATUS_BASE_DISAGREES_WITH_ACPI) {
        return false;
    }

    if (decode_version(UINT32_C(0x01050014), &version, &entries,
            &eoi_broadcast_suppression_supported) !=
            APIC_STATUS_OK ||
        version != UINT8_C(0x14) ||
        entries != UINT8_C(5) || !eoi_broadcast_suppression_supported) {
        return false;
    }

    if (decode_version(UINT32_C(0x00050014), &version, &entries,
            &eoi_broadcast_suppression_supported) != APIC_STATUS_OK ||
        eoi_broadcast_suppression_supported) {
        return false;
    }

    if (decode_version(UINT32_C(0x00050004), &version, &entries,
            &eoi_broadcast_suppression_supported) !=
        APIC_STATUS_EXTERNAL_APIC) {
        return false;
    }

    if (decode_version(UINT32_C(0x00020014), &version, &entries,
            &eoi_broadcast_suppression_supported) !=
        APIC_STATUS_TOO_FEW_LVT_ENTRIES) {
        return false;
    }

    acpi_bytes_zero(&topology, sizeof(topology));
    topology.local_apic_count = 1U;
    topology.local_apics[0].apic_id = 3U;
    topology.local_apics[0].enabled = false;

    if (check_identifier(&topology, 3U) !=
        APIC_STATUS_ID_DISAGREES_WITH_ACPI) {
        return false;
    }

    topology.local_apics[0].enabled = true;

    return check_identifier(&topology, 3U) == APIC_STATUS_OK &&
        check_identifier(&topology, 4U) == APIC_STATUS_ID_DISAGREES_WITH_ACPI &&
        apic_suppress_eoi_broadcasts() == APIC_STATUS_NOT_ONLINE &&
        apic_bring_online(NULL) == APIC_STATUS_NULL_ARGUMENT;
}

const char *apic_status_string(enum apic_status status)
{
    static const char *const messages[APIC_STATUS_COUNT] = {
        [APIC_STATUS_OK] = "ok",
        [APIC_STATUS_NULL_ARGUMENT] = "null local APIC argument",
        [APIC_STATUS_ALREADY_ONLINE] = "local APIC was brought online twice",
        [APIC_STATUS_INTERRUPTS_ENABLED] =
            "local APIC mutation requires interrupts disabled",
        [APIC_STATUS_UNSUPPORTED] = "processor reports no on-chip local APIC",
        [APIC_STATUS_HARDWARE_DISABLED] =
            "firmware left the local APIC hardware disabled",
        [APIC_STATUS_X2APIC_MODE] =
            "local APIC is in x2APIC mode and has no register window",
        [APIC_STATUS_NOT_BOOTSTRAP] =
            "local APIC bring-up ran off the bootstrap processor",
        [APIC_STATUS_NULL_BASE] = "local APIC base address is null",
        [APIC_STATUS_BASE_OUTSIDE_EARLY_MAP] =
            "local APIC base address is outside the early map",
        [APIC_STATUS_BASE_DISAGREES_WITH_ACPI] =
            "local APIC base disagrees with the ACPI MADT",
        [APIC_STATUS_EXTERNAL_APIC] =
            "local APIC reports a discrete 82489DX version",
        [APIC_STATUS_TOO_FEW_LVT_ENTRIES] =
            "local APIC lacks the required local vector table entries",
        [APIC_STATUS_ID_DISAGREES_WITH_ACPI] =
            "local APIC identifier is not an enabled ACPI processor",
        [APIC_STATUS_INTERRUPT_FAILURE] =
            "local APIC could not register its spurious handler",
        [APIC_STATUS_READBACK_MISMATCH] =
            "local APIC did not read back its programmed state",
        [APIC_STATUS_NOT_ONLINE] = "local APIC is not online",
        [APIC_STATUS_EOI_BROADCAST_SUPPRESSION_UNSUPPORTED] =
            "local APIC does not support EOI-broadcast suppression"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) == APIC_STATUS_COUNT,
        "local APIC status table must cover every enumerator"
    );

    if ((unsigned int)status >= APIC_STATUS_COUNT || messages[status] == NULL) {
        return "unknown local APIC status";
    }

    return messages[status];
}
