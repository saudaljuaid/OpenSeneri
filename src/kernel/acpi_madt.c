/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/acpi.h>
#include <phipia/acpi_util.h>

/* ACPI 6.6 table 5.20 assigns these interrupt-controller structure types. */
#define MADT_TYPE_LOCAL_APIC 0U
#define MADT_TYPE_IO_APIC 1U
#define MADT_TYPE_INTERRUPT_OVERRIDE 2U
#define MADT_TYPE_NMI_SOURCE 3U
#define MADT_TYPE_LOCAL_APIC_NMI 4U
#define MADT_TYPE_LOCAL_APIC_OVERRIDE 5U
#define MADT_TYPE_LOCAL_X2APIC 9U

/* ACPI 6.6 sections 5.2.12.2-5.2.12.12 fix each structure's length. */
#define MADT_ENTRY_HEADER_SIZE 2U
#define MADT_LOCAL_APIC_SIZE 8U
#define MADT_IO_APIC_SIZE 12U
#define MADT_INTERRUPT_OVERRIDE_SIZE 10U
#define MADT_NMI_SOURCE_SIZE 8U
#define MADT_LOCAL_APIC_NMI_SIZE 6U
#define MADT_LOCAL_APIC_OVERRIDE_SIZE 12U
#define MADT_LOCAL_X2APIC_SIZE 16U

/* ACPI 6.6 tables 5.22 and 5.26 define the flag fields Phipia accepts. */
#define MADT_PROCESSOR_ENABLED UINT32_C(1)
#define MADT_PROCESSOR_ONLINE_CAPABLE UINT32_C(2)
#define MADT_PROCESSOR_VALID_FLAGS \
    (MADT_PROCESSOR_ENABLED | MADT_PROCESSOR_ONLINE_CAPABLE)
#define MADT_INTI_VALID_FLAGS UINT16_C(0x000F)

/*
 * ACPI 6.6 section 5.2.12.5 defines interrupt source overrides only for the
 * ISA bus, whose sixteen legacy IRQs Phipia already owns through the 8259 pair.
 */
#define MADT_BUS_ISA 0U
#define MADT_ISA_IRQ_COUNT 16U

_Static_assert(ACPI_MAX_INTERRUPT_OVERRIDES >= MADT_ISA_IRQ_COUNT,
               "interrupt override storage no longer covers the ISA bus");

/* Intel SDM volume 3A sections 11.4.1 and 12.11.1 fix these register windows. */
#define LOCAL_APIC_WINDOW_SIZE UINT64_C(0x1000)
#define IO_APIC_WINDOW_SIZE UINT64_C(0x20)

static enum acpi_status revalidate_madt(
    const struct acpi_madt *madt,
    const uint8_t **table_out
)
{
    const uint8_t *table;

    if (madt->physical_address == 0U) {
        return ACPI_STATUS_NULL_TABLE;
    }

    if (madt->length < ACPI_MADT_FIXED_SIZE ||
        madt->length > ACPI_MAX_TABLE_SIZE) {
        return ACPI_STATUS_BAD_MADT_LENGTH;
    }

    if (!acpi_span_is_early_mapped(madt->physical_address, madt->length)) {
        return ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP;
    }

    table = (const uint8_t *)(uintptr_t)madt->physical_address;

    if (acpi_byte_sum(table, madt->length) != 0U) {
        return ACPI_STATUS_BAD_TABLE_CHECKSUM;
    }

    *table_out = table;
    return ACPI_STATUS_OK;
}

static enum acpi_status record_local_apic(
    struct acpi_topology *topology,
    uint32_t processor_uid,
    uint32_t apic_id,
    uint32_t flags
)
{
    struct acpi_local_apic *entry;

    if ((flags & ~MADT_PROCESSOR_VALID_FLAGS) != 0U) {
        return ACPI_STATUS_BAD_PROCESSOR_FLAGS;
    }

    for (size_t index = 0; index < topology->local_apic_count; ++index) {
        if (topology->local_apics[index].processor_uid == processor_uid) {
            return ACPI_STATUS_DUPLICATE_PROCESSOR_UID;
        }
    }

    if (topology->local_apic_count == ACPI_MAX_LOCAL_APICS) {
        return ACPI_STATUS_TOO_MANY_LOCAL_APICS;
    }

    entry = &topology->local_apics[topology->local_apic_count];
    entry->processor_uid = processor_uid;
    entry->apic_id = apic_id;
    entry->enabled = (flags & MADT_PROCESSOR_ENABLED) != 0U;
    entry->online_capable = (flags & MADT_PROCESSOR_ONLINE_CAPABLE) != 0U;
    ++topology->local_apic_count;

    if (entry->enabled) {
        ++topology->enabled_processor_count;
    }

    return ACPI_STATUS_OK;
}

static enum acpi_status record_io_apic(
    struct acpi_topology *topology,
    uint32_t identifier,
    uint32_t address,
    uint32_t interrupt_base
)
{
    struct acpi_io_apic *entry;

    if (address == 0U ||
        !acpi_span_is_early_mapped(address, IO_APIC_WINDOW_SIZE)) {
        return ACPI_STATUS_APIC_OUTSIDE_EARLY_MAP;
    }

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        if (topology->io_apics[index].identifier == identifier) {
            return ACPI_STATUS_DUPLICATE_IO_APIC_ID;
        }
    }

    if (topology->io_apic_count == ACPI_MAX_IO_APICS) {
        return ACPI_STATUS_TOO_MANY_IO_APICS;
    }

    entry = &topology->io_apics[topology->io_apic_count];
    entry->identifier = identifier;
    entry->address = address;
    entry->interrupt_base = interrupt_base;
    ++topology->io_apic_count;
    return ACPI_STATUS_OK;
}

static enum acpi_status record_interrupt_override(
    struct acpi_topology *topology,
    uint8_t bus,
    uint8_t source,
    uint32_t global_system_interrupt,
    uint16_t flags
)
{
    struct acpi_interrupt_override *entry;

    if (bus != MADT_BUS_ISA) {
        return ACPI_STATUS_BAD_OVERRIDE_BUS;
    }

    if (source >= MADT_ISA_IRQ_COUNT) {
        return ACPI_STATUS_BAD_OVERRIDE_SOURCE;
    }

    if ((flags & (uint16_t)~MADT_INTI_VALID_FLAGS) != 0U) {
        return ACPI_STATUS_BAD_OVERRIDE_FLAGS;
    }

    /*
     * The three checks above bound the recorded count by MADT_ISA_IRQ_COUNT,
     * which the static assert above ties to the array size. The count therefore
     * needs no separate capacity rejection.
     */
    for (size_t index = 0; index < topology->interrupt_override_count; ++index) {
        if (topology->interrupt_overrides[index].source == source) {
            return ACPI_STATUS_DUPLICATE_OVERRIDE_SOURCE;
        }
    }

    entry = &topology->interrupt_overrides[topology->interrupt_override_count];
    entry->bus = bus;
    entry->source = source;
    entry->global_system_interrupt = global_system_interrupt;
    entry->flags = flags;
    ++topology->interrupt_override_count;
    return ACPI_STATUS_OK;
}

static enum acpi_status record_local_apic_override(
    struct acpi_topology *topology,
    uint64_t address
)
{
    if (topology->local_apic_address_overridden) {
        return ACPI_STATUS_DUPLICATE_APIC_ADDRESS_OVERRIDE;
    }

    if (address == 0U ||
        !acpi_span_is_early_mapped(address, LOCAL_APIC_WINDOW_SIZE)) {
        return ACPI_STATUS_APIC_OUTSIDE_EARLY_MAP;
    }

    topology->local_apic_address = address;
    topology->local_apic_address_overridden = true;
    return ACPI_STATUS_OK;
}

/*
 * Consume one interrupt-controller structure. Every modelled type must declare
 * exactly its architectural length; a shorter one would read past the record
 * and a longer one means the firmware disagrees with the revision it claims.
 */
static enum acpi_status consume_entry(
    struct acpi_topology *topology,
    const uint8_t *entry,
    uint8_t length
)
{
    const uint8_t type = entry[0];
    size_t expected_length;

    switch (type) {
    case MADT_TYPE_LOCAL_APIC:
        expected_length = MADT_LOCAL_APIC_SIZE;
        break;
    case MADT_TYPE_IO_APIC:
        expected_length = MADT_IO_APIC_SIZE;
        break;
    case MADT_TYPE_INTERRUPT_OVERRIDE:
        expected_length = MADT_INTERRUPT_OVERRIDE_SIZE;
        break;
    case MADT_TYPE_NMI_SOURCE:
        expected_length = MADT_NMI_SOURCE_SIZE;
        break;
    case MADT_TYPE_LOCAL_APIC_NMI:
        expected_length = MADT_LOCAL_APIC_NMI_SIZE;
        break;
    case MADT_TYPE_LOCAL_APIC_OVERRIDE:
        expected_length = MADT_LOCAL_APIC_OVERRIDE_SIZE;
        break;
    case MADT_TYPE_LOCAL_X2APIC:
        expected_length = MADT_LOCAL_X2APIC_SIZE;
        break;
    default:
        ++topology->ignored_entry_count;
        return ACPI_STATUS_OK;
    }

    if ((size_t)length != expected_length) {
        return ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
    }

    switch (type) {
    case MADT_TYPE_LOCAL_APIC:
        return record_local_apic(
            topology,
            entry[2],
            entry[3],
            acpi_read_u32(entry + 4U)
        );
    case MADT_TYPE_IO_APIC:
        return record_io_apic(
            topology,
            entry[2],
            acpi_read_u32(entry + 4U),
            acpi_read_u32(entry + 8U)
        );
    case MADT_TYPE_INTERRUPT_OVERRIDE:
        return record_interrupt_override(
            topology,
            entry[2],
            entry[3],
            acpi_read_u32(entry + 4U),
            acpi_read_u16(entry + 8U)
        );
    case MADT_TYPE_LOCAL_APIC_OVERRIDE:
        return record_local_apic_override(topology, acpi_read_u64(entry + 4U));
    case MADT_TYPE_LOCAL_X2APIC:
        return record_local_apic(
            topology,
            acpi_read_u32(entry + 12U),
            acpi_read_u32(entry + 4U),
            acpi_read_u32(entry + 8U)
        );
    case MADT_TYPE_NMI_SOURCE:
    case MADT_TYPE_LOCAL_APIC_NMI:
    default:
        /* Length-checked and counted until an APIC driver consumes them. */
        ++topology->nmi_entry_count;
        return ACPI_STATUS_OK;
    }
}

enum acpi_status acpi_topology_discover(
    const struct acpi_madt *madt,
    struct acpi_topology *topology
)
{
    const uint8_t *table;
    enum acpi_status status;

    if (madt == NULL || topology == NULL) {
        return ACPI_STATUS_NULL_ARGUMENT;
    }

    acpi_bytes_zero(topology, sizeof(*topology));
    status = revalidate_madt(madt, &table);

    if (status != ACPI_STATUS_OK) {
        return status;
    }

    topology->local_apic_address = madt->local_apic_address;
    topology->legacy_pic_present =
        (madt->flags & ACPI_MADT_PCAT_COMPAT) != 0U;

    for (size_t offset = ACPI_MADT_FIXED_SIZE; offset < madt->length;) {
        const uint8_t *entry = table + offset;
        const size_t remaining = madt->length - offset;
        uint8_t length;

        if (remaining < MADT_ENTRY_HEADER_SIZE) {
            acpi_bytes_zero(topology, sizeof(*topology));
            return ACPI_STATUS_TRUNCATED_MADT_ENTRY;
        }

        length = entry[1];

        if (length < MADT_ENTRY_HEADER_SIZE) {
            acpi_bytes_zero(topology, sizeof(*topology));
            return ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
        }

        if ((size_t)length > remaining) {
            acpi_bytes_zero(topology, sizeof(*topology));
            return ACPI_STATUS_TRUNCATED_MADT_ENTRY;
        }

        status = consume_entry(topology, entry, length);

        if (status != ACPI_STATUS_OK) {
            acpi_bytes_zero(topology, sizeof(*topology));
            return status;
        }

        offset += length;
    }

    if (topology->enabled_processor_count == 0U) {
        acpi_bytes_zero(topology, sizeof(*topology));
        return ACPI_STATUS_MISSING_BOOT_PROCESSOR;
    }

    if (topology->io_apic_count == 0U) {
        acpi_bytes_zero(topology, sizeof(*topology));
        return ACPI_STATUS_MISSING_IO_APIC;
    }

    return ACPI_STATUS_OK;
}

/*
 * The in-kernel proof below builds synthetic MADTs on the stack. The kernel is
 * loaded low, so a fixture address is a valid identity-mapped physical address.
 */
#define TEST_MADT_CAPACITY 640U
#define TEST_LOCAL_APIC_ADDRESS UINT32_C(0xFEE00000)
#define TEST_IO_APIC_ADDRESS UINT32_C(0xFEC00000)
#define TEST_MADT_FLAGS UINT32_C(1)

struct topology_fixture {
    uint8_t bytes[TEST_MADT_CAPACITY];
    size_t length;
} __attribute__((aligned(8)));

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value & UINT16_C(0xFF));
    bytes[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = (uint8_t)((value >> (8U * index)) & UINT32_C(0xFF));
    }
}

static void write_u64(uint8_t *bytes, uint64_t value)
{
    write_u32(bytes, (uint32_t)value);
    write_u32(bytes + sizeof(uint32_t), (uint32_t)(value >> 32U));
}

static void fixture_begin(struct topology_fixture *fixture)
{
    static const char signature[4] = {'A', 'P', 'I', 'C'};

    acpi_bytes_zero(fixture, sizeof(*fixture));

    for (size_t index = 0; index < sizeof(signature); ++index) {
        fixture->bytes[index] = (uint8_t)signature[index];
    }

    fixture->bytes[8] = 5U;
    write_u32(fixture->bytes + 36U, TEST_LOCAL_APIC_ADDRESS);
    write_u32(fixture->bytes + 40U, TEST_MADT_FLAGS);
    fixture->length = ACPI_MADT_FIXED_SIZE;
}

static uint8_t *fixture_append(
    struct topology_fixture *fixture,
    uint8_t type,
    uint8_t length
)
{
    uint8_t *entry;

    if (length < MADT_ENTRY_HEADER_SIZE ||
        fixture->length + length > TEST_MADT_CAPACITY) {
        return NULL;
    }

    entry = fixture->bytes + fixture->length;
    entry[0] = type;
    entry[1] = length;
    fixture->length += length;
    return entry;
}

/* Recompute the declared length and checksum after building or mutating. */
static void fixture_seal(struct topology_fixture *fixture)
{
    write_u32(fixture->bytes + 4U, (uint32_t)fixture->length);
    fixture->bytes[9] = 0U;
    fixture->bytes[9] =
        (uint8_t)(0U - acpi_byte_sum(fixture->bytes, fixture->length));
}

static uint8_t *fixture_entry(struct topology_fixture *fixture, size_t wanted)
{
    size_t offset = ACPI_MADT_FIXED_SIZE;

    for (size_t position = 0; offset < fixture->length; ++position) {
        uint8_t *entry = fixture->bytes + offset;

        if (entry[1] < MADT_ENTRY_HEADER_SIZE) {
            return NULL;
        }

        if (position == wanted) {
            return entry;
        }

        offset += entry[1];
    }

    return NULL;
}

static struct acpi_madt fixture_madt(const struct topology_fixture *fixture)
{
    struct acpi_madt madt = {0};

    madt.physical_address =
        (uint64_t)(uintptr_t)(const void *)fixture->bytes;
    madt.length = (uint32_t)fixture->length;
    madt.local_apic_address = TEST_LOCAL_APIC_ADDRESS;
    madt.flags = TEST_MADT_FLAGS;
    madt.revision = 5U;
    return madt;
}

static uint8_t *append_local_apic(
    struct topology_fixture *fixture,
    uint8_t processor_uid,
    uint8_t apic_id,
    uint32_t flags
)
{
    uint8_t *entry =
        fixture_append(fixture, MADT_TYPE_LOCAL_APIC, MADT_LOCAL_APIC_SIZE);

    if (entry != NULL) {
        entry[2] = processor_uid;
        entry[3] = apic_id;
        write_u32(entry + 4U, flags);
    }

    return entry;
}

static uint8_t *append_io_apic(
    struct topology_fixture *fixture,
    uint8_t identifier,
    uint32_t address,
    uint32_t interrupt_base
)
{
    uint8_t *entry =
        fixture_append(fixture, MADT_TYPE_IO_APIC, MADT_IO_APIC_SIZE);

    if (entry != NULL) {
        entry[2] = identifier;
        write_u32(entry + 4U, address);
        write_u32(entry + 8U, interrupt_base);
    }

    return entry;
}

static uint8_t *append_override(
    struct topology_fixture *fixture,
    uint8_t source,
    uint32_t global_system_interrupt
)
{
    uint8_t *entry = fixture_append(
        fixture,
        MADT_TYPE_INTERRUPT_OVERRIDE,
        MADT_INTERRUPT_OVERRIDE_SIZE
    );

    if (entry != NULL) {
        entry[2] = MADT_BUS_ISA;
        entry[3] = source;
        write_u32(entry + 4U, global_system_interrupt);
        write_u16(entry + 8U, UINT16_C(0));
    }

    return entry;
}

/*
 * One enabled processor, one processor that is only online capable, one I/O
 * APIC, the customary IRQ0-to-GSI2 override, a local APIC NMI, and a structure
 * type Phipia does not model yet.
 */
static bool build_reference_fixture(struct topology_fixture *fixture)
{
    uint8_t *entry;

    fixture_begin(fixture);

    if (append_local_apic(fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL ||
        append_local_apic(
            fixture,
            1U,
            1U,
            MADT_PROCESSOR_ONLINE_CAPABLE
        ) == NULL ||
        append_io_apic(fixture, 0U, TEST_IO_APIC_ADDRESS, 0U) == NULL ||
        append_override(fixture, 0U, 2U) == NULL) {
        return false;
    }

    entry = fixture_append(
        fixture,
        MADT_TYPE_LOCAL_APIC_NMI,
        MADT_LOCAL_APIC_NMI_SIZE
    );

    if (entry == NULL) {
        return false;
    }

    entry[2] = UINT8_C(0xFF);
    write_u16(entry + 3U, UINT16_C(5));
    entry[5] = 1U;

    if (fixture_append(fixture, UINT8_C(0x7F), 4U) == NULL) {
        return false;
    }

    fixture_seal(fixture);
    return true;
}

static bool reference_fixture_is_accepted(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;

    if (!build_reference_fixture(&fixture)) {
        return false;
    }

    madt = fixture_madt(&fixture);

    return acpi_topology_discover(&madt, &topology) == ACPI_STATUS_OK &&
        topology.local_apic_address == TEST_LOCAL_APIC_ADDRESS &&
        !topology.local_apic_address_overridden &&
        topology.legacy_pic_present &&
        topology.local_apic_count == 2U &&
        topology.enabled_processor_count == 1U &&
        topology.local_apics[0].processor_uid == 0U &&
        topology.local_apics[0].enabled &&
        !topology.local_apics[0].online_capable &&
        topology.local_apics[1].processor_uid == 1U &&
        !topology.local_apics[1].enabled &&
        topology.local_apics[1].online_capable &&
        topology.io_apic_count == 1U &&
        topology.io_apics[0].address == TEST_IO_APIC_ADDRESS &&
        topology.io_apics[0].interrupt_base == 0U &&
        topology.interrupt_override_count == 1U &&
        topology.interrupt_overrides[0].bus == MADT_BUS_ISA &&
        topology.interrupt_overrides[0].source == 0U &&
        topology.interrupt_overrides[0].global_system_interrupt == 2U &&
        topology.nmi_entry_count == 1U &&
        topology.ignored_entry_count == 1U;
}

static bool x2apic_entry_is_accepted(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;
    uint8_t *entry;

    fixture_begin(&fixture);
    entry = fixture_append(
        &fixture,
        MADT_TYPE_LOCAL_X2APIC,
        MADT_LOCAL_X2APIC_SIZE
    );

    if (entry == NULL) {
        return false;
    }

    write_u32(entry + 4U, UINT32_C(0x01000000));
    write_u32(entry + 8U, MADT_PROCESSOR_ENABLED);
    write_u32(entry + 12U, UINT32_C(0x0000ABCD));

    if (append_io_apic(&fixture, 0U, TEST_IO_APIC_ADDRESS, 0U) == NULL) {
        return false;
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    return acpi_topology_discover(&madt, &topology) == ACPI_STATUS_OK &&
        topology.local_apic_count == 1U &&
        topology.enabled_processor_count == 1U &&
        topology.local_apics[0].processor_uid == UINT32_C(0x0000ABCD) &&
        topology.local_apics[0].apic_id == UINT32_C(0x01000000);
}

/* Mutate one byte range of the reference fixture and demand one rejection. */
static bool mutation_is_rejected(
    size_t entry_index,
    size_t byte_offset,
    uint8_t value,
    enum acpi_status expected
)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;
    uint8_t *entry;

    if (!build_reference_fixture(&fixture)) {
        return false;
    }

    entry = fixture_entry(&fixture, entry_index);

    if (entry == NULL) {
        return false;
    }

    entry[byte_offset] = value;
    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);
    return acpi_topology_discover(&madt, &topology) == expected;
}

static bool truncation_is_rejected(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;

    /* A trailing partial entry header cannot be walked. */
    fixture_begin(&fixture);

    if (append_local_apic(&fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL ||
        append_io_apic(&fixture, 0U, TEST_IO_APIC_ADDRESS, 0U) == NULL) {
        return false;
    }

    fixture.length += 1U;
    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_TRUNCATED_MADT_ENTRY) {
        return false;
    }

    /* A declared entry length may not run past the declared table. */
    return mutation_is_rejected(
        0U,
        1U,
        UINT8_C(0xFF),
        ACPI_STATUS_TRUNCATED_MADT_ENTRY
    );
}

static bool wrong_entry_length_is_rejected(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;

    fixture_begin(&fixture);

    if (fixture_append(
            &fixture,
            MADT_TYPE_LOCAL_APIC,
            MADT_LOCAL_APIC_SIZE + 2U
        ) == NULL) {
        return false;
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);
    return acpi_topology_discover(&madt, &topology) ==
        ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
}

static bool duplicate_structures_are_rejected(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;
    uint8_t *entry;

    fixture_begin(&fixture);

    if (append_local_apic(&fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL ||
        append_io_apic(&fixture, 3U, TEST_IO_APIC_ADDRESS, 0U) == NULL ||
        append_io_apic(&fixture, 3U, TEST_IO_APIC_ADDRESS, 24U) == NULL) {
        return false;
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_DUPLICATE_IO_APIC_ID) {
        return false;
    }

    fixture_begin(&fixture);

    if (append_local_apic(&fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL ||
        append_io_apic(&fixture, 0U, TEST_IO_APIC_ADDRESS, 0U) == NULL ||
        append_override(&fixture, 9U, 9U) == NULL ||
        append_override(&fixture, 9U, 11U) == NULL) {
        return false;
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_DUPLICATE_OVERRIDE_SOURCE) {
        return false;
    }

    fixture_begin(&fixture);

    if (append_local_apic(&fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL ||
        append_io_apic(&fixture, 0U, TEST_IO_APIC_ADDRESS, 0U) == NULL) {
        return false;
    }

    for (size_t repeat = 0; repeat < 2U; ++repeat) {
        entry = fixture_append(
            &fixture,
            MADT_TYPE_LOCAL_APIC_OVERRIDE,
            MADT_LOCAL_APIC_OVERRIDE_SIZE
        );

        if (entry == NULL) {
            return false;
        }

        write_u64(entry + 4U, TEST_LOCAL_APIC_ADDRESS);
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);
    return acpi_topology_discover(&madt, &topology) ==
        ACPI_STATUS_DUPLICATE_APIC_ADDRESS_OVERRIDE;
}

static bool apic_addresses_are_bounded(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;
    uint8_t *entry;

    /* A null I/O APIC window is not a register window. */
    fixture_begin(&fixture);

    if (append_local_apic(&fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL) {
        return false;
    }

    entry = append_io_apic(&fixture, 0U, 0U, 0U);

    if (entry == NULL) {
        return false;
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_APIC_OUTSIDE_EARLY_MAP) {
        return false;
    }

    /* A window that starts inside the early map may not run past its end. */
    write_u32(entry + 4U, UINT32_C(0xFFFFFFF0));
    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_APIC_OUTSIDE_EARLY_MAP) {
        return false;
    }

    fixture_begin(&fixture);

    if (append_local_apic(&fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL ||
        append_io_apic(&fixture, 0U, TEST_IO_APIC_ADDRESS, 0U) == NULL) {
        return false;
    }

    entry = fixture_append(
        &fixture,
        MADT_TYPE_LOCAL_APIC_OVERRIDE,
        MADT_LOCAL_APIC_OVERRIDE_SIZE
    );

    if (entry == NULL) {
        return false;
    }

    write_u64(entry + 4U, PHIPIA_EARLY_PHYSICAL_LIMIT);
    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_APIC_OUTSIDE_EARLY_MAP) {
        return false;
    }

    write_u64(entry + 4U, UINT64_C(0xFEE10000));
    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    return acpi_topology_discover(&madt, &topology) == ACPI_STATUS_OK &&
        topology.local_apic_address == UINT64_C(0xFEE10000) &&
        topology.local_apic_address_overridden;
}

static bool policy_bounds_are_enforced(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;

    fixture_begin(&fixture);

    if (append_io_apic(&fixture, 0U, TEST_IO_APIC_ADDRESS, 0U) == NULL) {
        return false;
    }

    for (size_t index = 0; index <= ACPI_MAX_LOCAL_APICS; ++index) {
        if (append_local_apic(
                &fixture,
                (uint8_t)index,
                (uint8_t)index,
                MADT_PROCESSOR_ENABLED
            ) == NULL) {
            return false;
        }
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_TOO_MANY_LOCAL_APICS) {
        return false;
    }

    fixture_begin(&fixture);

    if (append_local_apic(&fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL) {
        return false;
    }

    for (size_t index = 0; index <= ACPI_MAX_IO_APICS; ++index) {
        if (append_io_apic(
                &fixture,
                (uint8_t)index,
                TEST_IO_APIC_ADDRESS,
                (uint32_t)(index * 24U)
            ) == NULL) {
            return false;
        }
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);
    return acpi_topology_discover(&madt, &topology) ==
        ACPI_STATUS_TOO_MANY_IO_APICS;
}

static bool required_controllers_are_enforced(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;

    fixture_begin(&fixture);

    if (append_local_apic(
            &fixture,
            0U,
            0U,
            MADT_PROCESSOR_ONLINE_CAPABLE
        ) == NULL ||
        append_io_apic(&fixture, 0U, TEST_IO_APIC_ADDRESS, 0U) == NULL) {
        return false;
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_MISSING_BOOT_PROCESSOR) {
        return false;
    }

    fixture_begin(&fixture);

    if (append_local_apic(&fixture, 0U, 0U, MADT_PROCESSOR_ENABLED) == NULL) {
        return false;
    }

    fixture_seal(&fixture);
    madt = fixture_madt(&fixture);
    return acpi_topology_discover(&madt, &topology) ==
        ACPI_STATUS_MISSING_IO_APIC;
}

static bool recorded_table_is_revalidated(void)
{
    struct topology_fixture fixture;
    struct acpi_topology topology;
    struct acpi_madt madt;

    if (!build_reference_fixture(&fixture)) {
        return false;
    }

    madt = fixture_madt(&fixture);
    madt.physical_address = 0U;

    if (acpi_topology_discover(&madt, &topology) != ACPI_STATUS_NULL_TABLE) {
        return false;
    }

    madt = fixture_madt(&fixture);
    madt.length = ACPI_MADT_FIXED_SIZE - 1U;

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_BAD_MADT_LENGTH) {
        return false;
    }

    madt = fixture_madt(&fixture);
    madt.physical_address = PHIPIA_EARLY_PHYSICAL_LIMIT - madt.length + 1U;

    if (acpi_topology_discover(&madt, &topology) !=
        ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP) {
        return false;
    }

    fixture.bytes[9] ^= UINT8_C(1);
    madt = fixture_madt(&fixture);
    return acpi_topology_discover(&madt, &topology) ==
        ACPI_STATUS_BAD_TABLE_CHECKSUM;
}

bool acpi_topology_self_test(void)
{
    struct acpi_topology topology;

    if (acpi_topology_discover(NULL, &topology) != ACPI_STATUS_NULL_ARGUMENT) {
        return false;
    }

    return reference_fixture_is_accepted() &&
        x2apic_entry_is_accepted() &&
        truncation_is_rejected() &&
        wrong_entry_length_is_rejected() &&
        mutation_is_rejected(
            0U,
            1U,
            UINT8_C(1),
            ACPI_STATUS_BAD_MADT_ENTRY_LENGTH
        ) &&
        mutation_is_rejected(
            0U,
            4U,
            UINT8_C(4),
            ACPI_STATUS_BAD_PROCESSOR_FLAGS
        ) &&
        mutation_is_rejected(
            1U,
            2U,
            UINT8_C(0),
            ACPI_STATUS_DUPLICATE_PROCESSOR_UID
        ) &&
        mutation_is_rejected(
            3U,
            2U,
            UINT8_C(1),
            ACPI_STATUS_BAD_OVERRIDE_BUS
        ) &&
        mutation_is_rejected(
            3U,
            3U,
            UINT8_C(16),
            ACPI_STATUS_BAD_OVERRIDE_SOURCE
        ) &&
        mutation_is_rejected(
            3U,
            9U,
            UINT8_C(1),
            ACPI_STATUS_BAD_OVERRIDE_FLAGS
        ) &&
        duplicate_structures_are_rejected() &&
        apic_addresses_are_bounded() &&
        policy_bounds_are_enforced() &&
        required_controllers_are_enforced() &&
        recorded_table_is_revalidated();
}
